// File: Smartwatch_OS/main/main.cpp
#include <stdio.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "sdkconfig.h"

extern "C" {
    #include "ui_app.h"
}

#define TAG "main"

static i2c_master_dev_handle_t pmu_dev_handle = NULL;
static i2c_master_bus_handle_t pmu_bus_handle = NULL;

extern "C" {
    float battery_percentage = 0.0f;
    float battery_voltage = 0.0f;
    bool battery_present = false;
    bool battery_charging = false;
}

extern esp_err_t pmu_init();
extern void pmu_isr_handler();

// Brute forces every valid GPIO pin combo to find the PMU
void brute_force_i2c_pmu() {
    // Exclude SPI flash/PSRAM pins (26-37) and TX/RX pins (43, 44) to prevent instant crashes
    int safe_pins[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48};
    int num_pins = sizeof(safe_pins) / sizeof(safe_pins[0]);

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, " STARTING BRUTE-FORCE I2C SCAN FOR AXP2101 (0x34)");
    ESP_LOGW(TAG, "==================================================");

    for (int sda_idx = 0; sda_idx < num_pins; sda_idx++) {
        for (int scl_idx = 0; scl_idx < num_pins; scl_idx++) {
            if (sda_idx == scl_idx) continue;

            int sda = safe_pins[sda_idx];
            int scl = safe_pins[scl_idx];

            // Force internal pull-ups to prevent ESP_ERR_INVALID_STATE errors
            gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
            gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);

            i2c_master_bus_config_t i2c_mst_config = {};
            i2c_mst_config.i2c_port = -1;
            i2c_mst_config.sda_io_num = (gpio_num_t)sda;
            i2c_mst_config.scl_io_num = (gpio_num_t)scl;
            i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
            i2c_mst_config.glitch_ignore_cnt = 7;
            i2c_mst_config.flags.enable_internal_pullup = 1;

            i2c_master_bus_handle_t temp_bus;
            if (i2c_new_master_bus(&i2c_mst_config, &temp_bus) != ESP_OK) {
                continue;
            }

            // Probe the bus for the PMU (0x34)
            esp_err_t probe_ret = i2c_master_probe(temp_bus, 0x34, 50);

            if (probe_ret == ESP_OK) {
                ESP_LOGW(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                ESP_LOGW(TAG, "!! SUCCESS: FOUND AXP2101 PMU AT SDA = %d, SCL = %d !!", sda, scl);
                ESP_LOGW(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                
                pmu_bus_handle = temp_bus;

                // Register the PMU to this newly discovered bus
                i2c_device_config_t dev_config = {};
                dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
                dev_config.device_address = 0x34;
                dev_config.scl_speed_hz = 100000;
                dev_config.flags.disable_ack_check = 0;
                
                i2c_master_bus_add_device(pmu_bus_handle, &dev_config, &pmu_dev_handle);
                return; // Stop scanning!
            }

            // Not found on this combo, delete the bus and continue
            i2c_del_master_bus(temp_bus);
        }
    }
    ESP_LOGE(TAG, "BRUTE-FORCE SCAN FAILED! AXP2101 PMU (0x34) WAS NOT FOUND.");
}

void setup_pmu_i2c() {
    // 1. First, check if the PMU is just sharing the display's I2C bus (Very common)
    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus != NULL) {
        ESP_LOGI(TAG, "Checking if PMU is on the BSP's shared display I2C bus...");
        if (i2c_master_probe(bsp_bus, 0x34, 100) == ESP_OK) {
            ESP_LOGI(TAG, "--> YES! PMU found on the BSP shared I2C bus.");
            i2c_device_config_t dev_config = {};
            dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_config.device_address = 0x34;
            dev_config.scl_speed_hz = 100000;
            dev_config.flags.disable_ack_check = 0;
            i2c_master_bus_add_device(bsp_bus, &dev_config, &pmu_dev_handle);
            return;
        } else {
            ESP_LOGI(TAG, "--> NO. PMU is not on the BSP bus.");
        }
    }

    // 2. If it's not on the display bus, run the brute-force scanner
    brute_force_i2c_pmu();
}

int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    esp_err_t ret = i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Read Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, -1);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Write Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

static void pmu_hander_task(void *args) {
    while (1) {
        pmu_isr_handler();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Starting Smartwatch OS...");

    // 1. Initialize display and UI FIRST. This ensures the BSP configures its own I2C properly.
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    // 2. Automatically locate and map the PMU I2C pins
    setup_pmu_i2c();

    // 3. Start the PMU
    if (pmu_dev_handle != NULL) {
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
            ESP_LOGI(TAG, "PMU successfully activated!");
        } else {
            ESP_LOGE(TAG, "PMU init failed! Proceeding without battery monitor.");
        }
    } else {
        ESP_LOGE(TAG, "PMU completely unresponsive. Proceeding without battery monitor.");
    }
}
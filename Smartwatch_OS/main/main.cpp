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

#define I2C_MASTER_TIMEOUT_MS 1000

static i2c_master_dev_handle_t pmu_dev_handle = NULL;
static i2c_master_bus_handle_t pmu_bus_handle = NULL;

extern "C" {
    float battery_percentage = 0.0f;
    float battery_voltage = 0.0f;
    bool battery_present = false;
    bool battery_charging = false;
}

// Function declarations from port_axp2101.cpp
extern esp_err_t pmu_init();
extern void pmu_isr_handler();

// Clears the specified I2C bus pins to release stuck slaves
void i2c_clear_bus(int sda, int scl) {
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
    gpio_set_direction((gpio_num_t)sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)scl, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);
    
    gpio_set_level((gpio_num_t)sda, 1);
    gpio_set_level((gpio_num_t)scl, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Toggle SCL 9 times to free any stuck slaves
    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)scl, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level((gpio_num_t)scl, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Verifies device is a real AXP2101 PMU by reading its physical Chip ID register (0x03)
esp_err_t verify_pmu(i2c_master_dev_handle_t dev) {
    uint8_t reg = 0x03;
    uint8_t chip_id = 0;
    // Timeout of 100ms is more than enough for a register read
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, &chip_id, 1, 100);
    if (ret == ESP_OK && chip_id == 0x4A) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

// Brute-force scanner that tests safe GPIO pins
esp_err_t brute_force_pmu_scan(int *out_sda, int *out_scl) {
    // Candidate safe pins (excluding PSRAM/Flash registers), starting with the most likely
    int safe_pins[] = {15, 14, 6, 7, 8, 9, 4, 5, 10, 11, 12, 13, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48};
    int num_pins = sizeof(safe_pins) / sizeof(safe_pins[0]);

    ESP_LOGI(TAG, "Starting robust brute-force PMU scanner...");

    for (int sda_idx = 0; sda_idx < num_pins; sda_idx++) {
        for (int scl_idx = 0; scl_idx < num_pins; scl_idx++) {
            if (sda_idx == scl_idx) continue;

            int sda = safe_pins[sda_idx];
            int scl = safe_pins[scl_idx];

            i2c_clear_bus(sda, scl);

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

            i2c_device_config_t dev_config = {};
            dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_config.device_address = 0x34; // AXP2101 Address
            dev_config.scl_speed_hz = 100000;

            i2c_master_dev_handle_t temp_dev;
            if (i2c_master_bus_add_device(temp_bus, &dev_config, &temp_dev) != ESP_OK) {
                i2c_del_master_bus(temp_bus);
                continue;
            }

            if (verify_pmu(temp_dev) == ESP_OK) {
                ESP_LOGW(TAG, "SUCCESS: Found verified AXP2101 PMU on SDA=%d, SCL=%d", sda, scl);
                *out_sda = sda;
                *out_scl = scl;
                
                // Save handles for setup
                pmu_dev_handle = temp_dev;
                pmu_bus_handle = temp_bus;
                return ESP_OK;
            }

            // Clean up temporary device and bus for next attempt
            i2c_master_bus_rm_device(temp_dev);
            i2c_del_master_bus(temp_bus);
        }
    }

    return ESP_ERR_NOT_FOUND;
}

// PMU read function utilizing the active I2C handle
int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    esp_err_t ret = i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Read Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

// PMU write function utilizing the active I2C handle
int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
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

    int pmu_sda = -1;
    int pmu_scl = -1;

    // 1. Scan and verify PMU on active hardware
    if (brute_force_pmu_scan(&pmu_sda, &pmu_scl) == ESP_OK) {
        // 2. Initialize PMU to enable system voltage rails
        if (pmu_init() == ESP_OK) {
            ESP_LOGI(TAG, "PMU successfully initialized on scanned bus.");
        } else {
            ESP_LOGE(TAG, "Failed to run PMU init!");
        }

        // 3. Clean up the scanner's temporary bus to free up the GPIO pins
        i2c_master_bus_rm_device(pmu_dev_handle);
        i2c_del_master_bus(pmu_bus_handle);
        pmu_dev_handle = NULL;
        pmu_bus_handle = NULL;
    } else {
        ESP_LOGE(TAG, "PMU completely absent or unresponsive!");
    }

    // Allow PMU voltage rails to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));

    // 4. Force pull-ups on the watch's physical I2C pins so the BSP boots cleanly
    gpio_set_pull_mode((gpio_num_t)15, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)14, GPIO_PULLUP_ONLY);

    // 5. Initialize display and UI (shared I2C initializes safely here)
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    // 6. Permanently bind PMU device onto the BSP's shared I2C bus
    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus != NULL) {
        i2c_device_config_t dev_config = {};
        dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_config.device_address = 0x34;
        dev_config.scl_speed_hz = 100000;

        esp_err_t ret = i2c_master_bus_add_device(bsp_bus, &dev_config, &pmu_dev_handle);
        if (ret == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
            ESP_LOGI(TAG, "PMU bound permanently to the shared display bus.");
        } else {
            ESP_LOGE(TAG, "Failed to bind PMU permanently!");
        }
    } else {
        ESP_LOGE(TAG, "BSP bus handle not available!");
    }
}
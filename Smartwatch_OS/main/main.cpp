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

// Hardware I2C pins for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch
#define I2C_SDA_IO     15
#define I2C_SCL_IO     14

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

// Scans the entire I2C address space on pins 15 & 14 to locate all alive devices
esp_err_t scan_i2c_bus() {
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, " DIAGNOSTIC I2C ADDRESS SCAN ON PINS 15 & 14... ");
    ESP_LOGW(TAG, "==================================================");

    i2c_clear_bus(I2C_SDA_IO, I2C_SCL_IO);

    i2c_master_bus_config_t i2c_mst_config = {};
    i2c_mst_config.i2c_port = I2C_NUM_0; // Force Port 0
    i2c_mst_config.sda_io_num = (gpio_num_t)I2C_SDA_IO;
    i2c_mst_config.scl_io_num = (gpio_num_t)I2C_SCL_IO;
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = 1; // Enable internal pull-ups

    esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, &pmu_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus for scanning! Err: %s", esp_err_to_name(ret));
        return ret;
    }

    bool pmu_found = false;
    for (uint8_t addr = 1; addr < 128; addr++) {
        esp_err_t probe_ret = i2c_master_probe(pmu_bus_handle, addr, 50);
        if (probe_ret == ESP_OK) {
            ESP_LOGW(TAG, "Found responsive I2C device at Address: 0x%02X (%d)", addr, addr);
            if (addr == 0x34) {
                pmu_found = true;
            }
        }
    }
    ESP_LOGW(TAG, "==================================================");

    if (pmu_found) {
        // Register PMU on this successful bus
        i2c_device_config_t dev_config = {};
        dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_config.device_address = 0x34; // AXP2101 Address
        dev_config.scl_speed_hz = 100000;  // 100 kHz for stability

        ret = i2c_master_bus_add_device(pmu_bus_handle, &dev_config, &pmu_dev_handle);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    }

    // Clean up bus if PMU was not found
    i2c_del_master_bus(pmu_bus_handle);
    pmu_bus_handle = NULL;
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

    // 1. Scan and register PMU first using the custom address scanner on the real pins (15/14)
    if (scan_i2c_bus() == ESP_OK) {
        // 2. Initialize PMU to enable system voltage rails
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
            ESP_LOGI(TAG, "PMU successfully initialized on the custom bus.");
        } else {
            ESP_LOGE(TAG, "PMU hardware configuration failed!");
        }
    } else {
        ESP_LOGE(TAG, "PMU was not found at 0x34 during address scan!");
    }

    // Allow PMU voltage rails to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. Initialize display and UI. 
    // Since we forced our scanner to Port 0 and left the bus open, bsp_display_start
    // will see that Port 0 is already active, skip its own config, and cleanly reuse our bus!
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();
}
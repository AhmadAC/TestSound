// File: Smartwatch_OS/main/main.cpp
#include <stdio.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c.h"     // Use the legacy I2C driver for BSP compatibility
#include "bsp/esp-bsp.h"
#include "sdkconfig.h"

extern "C" {
    #include "ui_app.h"
}

#define TAG "main"

// Dedicated PMU I2C pins (from Waveshare Schematic)
#define I2C_PMU_PORT       I2C_NUM_1
#define I2C_PMU_SDA_IO     6
#define I2C_PMU_SCL_IO     7
#define I2C_MASTER_FREQ_HZ 400000
#define I2C_MASTER_TIMEOUT_MS 1000

// Define the global battery metrics with C-linkage so ui_app.c can read them
extern "C" {
    float battery_percentage = 0.0f;
    float battery_voltage = 0.0f;
    bool battery_present = false;
    bool battery_charging = false;
}

// Function declarations from port_axp2101.cpp
extern esp_err_t pmu_init();
extern void pmu_isr_handler();

// Dedicated I2C bus initialization for PMU using the robust legacy driver
esp_err_t i2c_init() {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_PMU_SDA_IO;
    conf.scl_io_num = I2C_PMU_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE; 
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    conf.clk_flags = 0;

    esp_err_t ret = i2c_param_config(I2C_PMU_PORT, &conf);
    if (ret != ESP_OK) return ret;

    return i2c_driver_install(I2C_PMU_PORT, conf.mode, 0, 0, 0);
}

// PMU read function utilizing the legacy I2C API
int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    esp_err_t ret = i2c_master_write_read_device(I2C_PMU_PORT, devAddr, &regAddr, 1, data, len, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Read Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

// PMU write function utilizing the legacy I2C API
int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_write_to_device(I2C_PMU_PORT, devAddr, buffer, len + 1, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Write Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

// Background task to poll the battery metrics
static void pmu_hander_task(void *args) {
    while (1) {
        pmu_isr_handler();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Starting Smartwatch OS display and peripherals...");

    // Initialize display and UI
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    // Connect to the dedicated PMU I2C bus and init PMU
    if (i2c_init() == ESP_OK) {
        ESP_LOGI(TAG, "Dedicated PMU I2C bus initialized on Port 1.");
        
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
        } else {
            ESP_LOGE(TAG, "PMU init failed! Proceeding without battery monitor to avoid bootloop.");
        }
    } else {
        ESP_LOGE(TAG, "I2C Init failed! Proceeding without battery monitor.");
    }
}
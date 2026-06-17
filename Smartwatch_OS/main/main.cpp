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

// Known I2C pins for the Waveshare ESP32-S3-Touch-AMOLED-2.06
#define I2C_SDA_IO     15
#define I2C_SCL_IO     14

static i2c_master_dev_handle_t pmu_dev_handle = NULL;

extern "C" {
    float battery_percentage = 0.0f;
    float battery_voltage = 0.0f;
    bool battery_present = false;
    bool battery_charging = false;
}

extern esp_err_t pmu_init();
extern void pmu_isr_handler();

esp_err_t i2c_init() {
    // 1. Force internal pull-ups to ensure stable I2C lines
    gpio_set_pull_mode((gpio_num_t)I2C_SDA_IO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)I2C_SCL_IO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. Initialize the BSP's shared I2C bus
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bsp_i2c_init failed! Err: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Retrieve the initialized master bus handle
    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus == NULL) {
        ESP_LOGE(TAG, "Failed to retrieve BSP I2C master bus!");
        return ESP_FAIL;
    }

    // 4. Add the AXP2101 PMU device to the shared bus
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = 0x34; // AXP2101 I2C Address
    dev_config.scl_speed_hz = 100000; // 100 kHz is safe and reliable
    dev_config.scl_wait_us = 0;
    dev_config.flags.disable_ack_check = 0;

    ret = i2c_master_bus_add_device(bsp_bus, &dev_config, &pmu_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PMU I2C device! Err: %s", esp_err_to_name(ret));
    }
    return ret;
}

int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    // Timeout -1 ensures we wait for completion
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

    // Timeout -1 ensures we wait for completion
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
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Starting Smartwatch OS...");

    // Initialize I2C and PMU FIRST so power rails are turned on for the display
    if (i2c_init() == ESP_OK) {
        ESP_LOGI(TAG, "Shared BSP I2C bus ready and PMU registered.");
        
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
            ESP_LOGI(TAG, "PMU active. Powering display rails...");
        } else {
            ESP_LOGE(TAG, "PMU init failed! Proceeding without battery monitor.");
        }
    } else {
        ESP_LOGE(TAG, "I2C Init failed!");
    }

    // Allow power rails to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));

    // Now start the display and UI
    ESP_LOGI(TAG, "Initializing Display and LVGL UI...");
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();
}
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

// Config defaults for PMU specific I2C pins (According to Waveshare Schematic)
#define I2C_PMU_SDA_IO 6
#define I2C_PMU_SCL_IO 7
#define I2C_MASTER_FREQ_HZ 100000
#define I2C_MASTER_TIMEOUT_MS 1000

static i2c_master_dev_handle_t pmu_dev_handle = NULL;

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

// Dedicated I2C bus initialization for PMU
esp_err_t i2c_init() {
    i2c_master_bus_config_t i2c_mst_config = {};
    i2c_mst_config.i2c_port = -1; // Auto-select an available I2C port
    i2c_mst_config.sda_io_num = (gpio_num_t)I2C_PMU_SDA_IO;
    i2c_mst_config.scl_io_num = (gpio_num_t)I2C_PMU_SCL_IO;
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = 1;

    i2c_master_bus_handle_t pmu_bus_handle;
    esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, &pmu_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PMU I2C master bus!");
        return ret;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = 0x34; // AXP2101 I2C Address
    dev_config.scl_speed_hz = I2C_MASTER_FREQ_HZ;
    dev_config.scl_wait_us = 0;
    dev_config.flags.disable_ack_check = 0;

    ret = i2c_master_bus_add_device(pmu_bus_handle, &dev_config, &pmu_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PMU I2C device!");
    }
    return ret;
}

// PMU read function utilizing the custom I2C handle
int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    esp_err_t ret = i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Read Register 0x%02X Failed!", regAddr);
        return -1;
    }
    return 0;
}

// PMU write function utilizing the custom I2C handle
int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Write Register 0x%02X Failed!", regAddr);
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
        ESP_LOGI(TAG, "Dedicated PMU I2C bus initialized.");
        
        // Removed ESP_ERROR_CHECK to prevent watch bootloops on bad battery states
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
        } else {
            ESP_LOGE(TAG, "PMU init failed! Proceeding without battery monitor.");
        }
    } else {
        ESP_LOGE(TAG, "I2C Init failed! Proceeding without battery monitor.");
    }
}
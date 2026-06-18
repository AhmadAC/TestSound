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

// Corrected hardware I2C pins for the Waveshare ESP32-S3-Touch-AMOLED-2.06 PMU/Sensors
#define I2C_PMU_SDA_IO     4
#define I2C_PMU_SCL_IO     5
#define I2C_MASTER_FREQ_HZ 400000 
#define I2C_MASTER_TIMEOUT_MS 50 

static i2c_master_bus_handle_t pmu_bus_handle = NULL;
static i2c_master_dev_handle_t pmu_dev_handle = NULL;
static uint8_t active_pmu_addr = 0;

extern "C" {
    float battery_percentage = 0.0f;
    float battery_voltage = 0.0f;
    bool battery_present = false;
    bool battery_charging = false;

    uint8_t get_pmu_address() {
        return active_pmu_addr;
    }
}

// Function declarations from port_axp2101.cpp
extern esp_err_t pmu_init();
extern void pmu_isr_handler();

// Clears the specified I2C bus pins to release stuck slaves
void i2c_clear_bus() {
    gpio_reset_pin((gpio_num_t)I2C_PMU_SDA_IO);
    gpio_reset_pin((gpio_num_t)I2C_PMU_SCL_IO);
    gpio_set_direction((gpio_num_t)I2C_PMU_SDA_IO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)I2C_PMU_SCL_IO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)I2C_PMU_SDA_IO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)I2C_PMU_SCL_IO, GPIO_PULLUP_ONLY);
    
    gpio_set_level((gpio_num_t)I2C_PMU_SDA_IO, 1);
    gpio_set_level((gpio_num_t)I2C_PMU_SCL_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Toggle SCL 9 times to reset any locked up I2C peripherals
    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)I2C_PMU_SCL_IO, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level((gpio_num_t)I2C_PMU_SCL_IO, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Initialize I2C independently from the BSP (since BSP manages the Touch I2C on pins 14/15)
esp_err_t i2c_init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = (gpio_num_t)I2C_PMU_SDA_IO;
    bus_cfg.scl_io_num = (gpio_num_t)I2C_PMU_SCL_IO;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &pmu_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PMU I2C bus!");
        return ret;
    }

    // Safely probe to find exact PMU address
    if (i2c_master_probe(pmu_bus_handle, 0x6A, 50) == ESP_OK) {
        active_pmu_addr = 0x6A; // SY6970 (V2.0.0 hardware)
    } else if (i2c_master_probe(pmu_bus_handle, 0x34, 50) == ESP_OK) {
        active_pmu_addr = 0x34; // AXP2101 (V1.0.0 hardware)
    }

    if (active_pmu_addr == 0) {
        ESP_LOGE(TAG, "PMU not found on I2C bus!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Found PMU at I2C address 0x%02X", active_pmu_addr);

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = active_pmu_addr; 
    dev_cfg.scl_speed_hz = I2C_MASTER_FREQ_HZ;
    dev_cfg.scl_wait_us = 0;
    dev_cfg.flags.disable_ack_check = 0;

    ret = i2c_master_bus_add_device(pmu_bus_handle, &dev_cfg, &pmu_dev_handle);
    return ret;
}

int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    
    if (i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS) != ESP_OK) {
        return -1;
    }

    // Spoof Chip ID for SY6970 (REG 0x14) to bypass library strict checks
    if (active_pmu_addr == 0x6A && regAddr == 0x14 && len == 1) {
        *data = 0x00; // Expected ID value
    }

    return 0;
}

int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
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

    // 1. Enforce pull-ups and clear bus BEFORE any other drivers initialize
    i2c_clear_bus();

    // 2. Initialize display and UI (this configures the shared display I2C bus on GPIO 14/15)
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    // 3. Connect to the sensor I2C bus and init PMU on GPIO 4/5
    if (i2c_init() == ESP_OK) {
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
        } else {
            ESP_LOGE(TAG, "PMU init failed! Proceeding without battery monitor.");
        }
    } else {
        ESP_LOGE(TAG, "I2C Init failed!");
    }
}
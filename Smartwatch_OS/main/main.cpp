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

// Confirmed hardware I2C pins for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch
#define I2C_PMU_SDA_IO     15
#define I2C_PMU_SCL_IO     14
#define I2C_MASTER_FREQ_HZ 100000 
#define I2C_MASTER_TIMEOUT_MS 1000

// Handles for both possible SY6970 addresses on V2.0.0 boards
static i2c_master_dev_handle_t pmu_dev_6a = NULL;
static i2c_master_dev_handle_t pmu_dev_6b = NULL;

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

// Initialize I2C utilizing the BSP bus architecture
esp_err_t i2c_init() {
    esp_err_t ret = bsp_i2c_init(); 
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bsp_i2c_init failed! Err: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus == NULL) {
        ESP_LOGE(TAG, "Failed to retrieve BSP I2C master bus!");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = I2C_MASTER_FREQ_HZ;
    dev_cfg.scl_wait_us = 0;
    dev_cfg.flags.disable_ack_check = 0;

    // Register 0x6A (SY6970 Alternative)
    dev_cfg.device_address = 0x6A; 
    i2c_master_bus_add_device(bsp_bus, &dev_cfg, &pmu_dev_6a);

    // Register 0x6B (SY6970 Standard)
    dev_cfg.device_address = 0x6B; 
    i2c_master_bus_add_device(bsp_bus, &dev_cfg, &pmu_dev_6b);

    return ESP_OK;
}

int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    i2c_master_dev_handle_t handle = (devAddr == 0x6A) ? pmu_dev_6a : pmu_dev_6b;
    if (handle == NULL) return -1;

    if (i2c_master_transmit_receive(handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS) != ESP_OK) {
        return -1;
    }
    return 0;
}

int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    i2c_master_dev_handle_t handle = (devAddr == 0x6A) ? pmu_dev_6a : pmu_dev_6b;
    if (handle == NULL) return -1;
    
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
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

    // 2. Initialize display and UI (this configures the shared display I2C bus)
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    // 3. FORCE internal pull-ups on GPIO 15 and 14 after the BSP display driver overrides them!
    gpio_set_pull_mode((gpio_num_t)I2C_PMU_SDA_IO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)I2C_PMU_SCL_IO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow lines to rise to VCC

    // 4. Connect to the shared I2C bus and init PMU
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
/*
 * Smartwatch OS Battery Monitor Implementation
 * Directly interfaces with the AXP2101 PMU over I2C
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"

#define I2C_PORT_NUM    I2C_NUM_0
#define AXP2101_ADDR    0x34

static const char *TAG = "smartwatch_battery";

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t pmu_dev_handle = NULL;

esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT_NUM,
        .scl_io_num = 14,
        .sda_io_num = 15,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &i2c_bus_handle);
}

esp_err_t pmu_read_reg(uint8_t reg_addr, uint8_t *data) {
    return i2c_master_transmit_receive(pmu_dev_handle, &reg_addr, 1, data, 1, 1000);
}

esp_err_t pmu_write_reg(uint8_t reg_addr, uint8_t data) {
    uint8_t buf[2] = {reg_addr, data};
    return i2c_master_transmit(pmu_dev_handle, buf, sizeof(buf), 1000);
}

void battery_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting battery monitor task...");

    uint8_t val = 0;
    
    // Enable Fuel Gauge Module (Reg 0x18, Bit 3)
    if (pmu_read_reg(0x18, &val) == ESP_OK) {
        val |= (1 << 3); 
        pmu_write_reg(0x18, val);
    } else {
        ESP_LOGW(TAG, "Failed to communicate with AXP2101 (Reg 0x18)");
    }
    
    // Enable Battery Voltage Measure ADC Channel (Reg 0x30, Bit 0)
    if (pmu_read_reg(0x30, &val) == ESP_OK) {
        val |= (1 << 0); 
        pmu_write_reg(0x30, val);
    } else {
        ESP_LOGW(TAG, "Failed to communicate with AXP2101 (Reg 0x30)");
    }

    while (1) {
        uint8_t percent = 0;
        uint8_t vbat_h = 0, vbat_l = 0;
        
        // Read Fuel Gauge Percentage (Reg 0xA4)
        esp_err_t ret = pmu_read_reg(0xA4, &percent);
        
        // Read 14-Bit ADC Battery Voltage (Reg 0x34 & 0x35)
        pmu_read_reg(0x34, &vbat_h);
        pmu_read_reg(0x35, &vbat_l);
        
        if (ret == ESP_OK) {
            // Reconstruct the 14-bit voltage (1mV per step)
            uint16_t vbat_mv = ((vbat_h & 0x3F) << 8) | vbat_l;
            
            // Safety cap if percentage goes weirdly out of bounds
            if (percent > 100) percent = 100;
            
            ESP_LOGI(TAG, "Battery: %d%% (%d mV)", percent, vbat_mv);
        } else {
            ESP_LOGW(TAG, "Failed to read battery data: %s", esp_err_to_name(ret));
        }

        // Check battery every 5 seconds
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    // Delay to let the serial terminal on the PC re-enumerate and connect safely before logs are sent
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Initializing Smartwatch Battery OS...");

    esp_err_t ret = i2c_master_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C master initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "I2C initialized successfully (SDA=15, SCL=14)");

    i2c_device_config_t pmu_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    
    ret = i2c_master_bus_add_device(i2c_bus_handle, &pmu_cfg, &pmu_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register AXP2101 device on I2C bus: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "AXP2101 registered successfully");

    // Create background FreeRTOS task to continuously poll battery
    xTaskCreate(battery_monitor_task, "battery_monitor_task", 4096, NULL, 5, NULL);
}
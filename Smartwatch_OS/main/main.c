// File: Smartwatch_OS/main/main.c
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
#include "bsp/esp-bsp.h"
#include "ui_app.h"

#define AXP2101_ADDR    0x34

static const char *TAG = "smartwatch_battery";

static i2c_master_dev_handle_t pmu_dev_handle = NULL;

// Global metrics used directly by ui_app.c
float battery_percentage = 0.0f;
float battery_voltage = 0.0f;
bool battery_present = false;
bool battery_charging = false;

esp_err_t pmu_read_reg(uint8_t reg_addr, uint8_t *data) {
    if (pmu_dev_handle == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(pmu_dev_handle, &reg_addr, 1, data, 1, 1000);
}

esp_err_t pmu_write_reg(uint8_t reg_addr, uint8_t data) {
    if (pmu_dev_handle == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg_addr, data};
    return i2c_master_transmit(pmu_dev_handle, buf, sizeof(buf), 1000);
}

void battery_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting battery monitor task...");

    uint8_t val = 0;
    
    // 1. Enable Battery Detection (Reg 0x68, Bit 0)
    if (pmu_read_reg(0x68, &val) == ESP_OK) {
        pmu_write_reg(0x68, val | 0x01);
    }

    // 2. Enable Fuel Gauge Module & Charger (Reg 0x18, Bit 3 and Bit 1)
    if (pmu_read_reg(0x18, &val) == ESP_OK) {
        pmu_write_reg(0x18, val | 0x0A); 
    }
    
    // 3. Enable ADC Channels for VBAT, VBUS, VSYS (Reg 0x30, Bits 0, 1, 2)
    // CRITICAL: We MUST disable the TS (Temperature Sensor) pin (Bits 3/4) 
    // otherwise the PMU faults and stops charging/reading because the board lacks a thermistor.
    if (pmu_read_reg(0x30, &val) == ESP_OK) {
        // Keep highest bits, clear the middle (TS/GPADC), and set bottom 3 for voltage ADCs
        pmu_write_reg(0x30, (val & 0xC0) | 0x07); 
    }

    while (1) {
        uint8_t status1 = 0;
        uint8_t percent = 0;
        uint8_t vbat_h = 0;
        uint8_t vbat_l = 0;
        
        pmu_read_reg(0x00, &status1);
        pmu_read_reg(0xA4, &percent);
        pmu_read_reg(0x34, &vbat_h);
        pmu_read_reg(0x35, &vbat_l);
        
        // Reconstruct the 14-bit voltage (1mV per step on AXP2101)
        uint16_t vbat_raw = ((vbat_h & 0x3F) << 8) | vbat_l;
        float voltage = (float)vbat_raw / 1000.0f;
        
        // VBUS Good = Bit 5 of Status 1 (Is USB Power Connected?)
        bool vbus_good = (status1 & (1 << 5)) != 0;
        
        // Map to globals
        battery_voltage = voltage;
        battery_percentage = (percent > 100) ? 100.0f : (float)percent;
        
        // Fallback: If the PMU fuel gauge hasn't calibrated yet but the voltage is high enough,
        // calculate a manual percentage so the UI doesn't say 0%. (3.3V = 0%, 4.2V = 100%)
        if (battery_percentage == 0.0f && battery_voltage > 3.0f) {
            battery_percentage = ((battery_voltage - 3.3f) / 0.9f) * 100.0f;
            if (battery_percentage > 100.0f) battery_percentage = 100.0f;
            if (battery_percentage < 0.0f) battery_percentage = 0.0f;
        }

        // Trust the raw ADC: If the voltage is above 2.0V, the battery is physically connected.
        battery_present = (battery_voltage > 2.0f);
        
        // Charging logic maps to USB presence
        battery_charging = vbus_good;
        
        ESP_LOGI(TAG, "Battery: %d%% (%.3f V) [Present: %d, USB: %d]", 
                 (int)battery_percentage, battery_voltage, battery_present, battery_charging);

        // Poll every 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Initializing Smartwatch OS display and peripherals...");

    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    ESP_LOGI(TAG, "Connecting to shared I2C bus...");

    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus == NULL) {
        bsp_i2c_init();
        bsp_bus = bsp_i2c_get_handle();
    }

    if (bsp_bus != NULL) {
        i2c_device_config_t pmu_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 100000,
        };
        
        esp_err_t ret = i2c_master_bus_add_device(bsp_bus, &pmu_cfg, &pmu_dev_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "AXP2101 registered successfully on shared BSP I2C bus");
        }
    }

    xTaskCreate(battery_monitor_task, "battery_monitor_task", 4096, NULL, 5, NULL);
}
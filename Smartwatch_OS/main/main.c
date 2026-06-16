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
    if (pmu_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(pmu_dev_handle, &reg_addr, 1, data, 1, 1000);
}

esp_err_t pmu_write_reg(uint8_t reg_addr, uint8_t data) {
    if (pmu_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg_addr, data};
    return i2c_master_transmit(pmu_dev_handle, buf, sizeof(buf), 1000);
}

void battery_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting battery monitor task...");

    uint8_t val = 0;
    
    // 1. Enable Battery Detection (Reg 0x68, Bit 0)
    if (pmu_read_reg(0x68, &val) == ESP_OK) {
        val |= 0x01; // Enable battery detection
        pmu_write_reg(0x68, val);
        ESP_LOGI(TAG, "Battery detection configured: 0x%02X", val);
    } else {
        ESP_LOGE(TAG, "Failed to communicate with AXP2101 (Reg 0x68)");
    }

    // 2. Enable Fuel Gauge Module & Charger (Reg 0x18, Bit 3 and Bit 1)
    if (pmu_read_reg(0x18, &val) == ESP_OK) {
        val |= (1 << 3) | (1 << 1); 
        pmu_write_reg(0x18, val);
        ESP_LOGI(TAG, "Fuel Gauge & Charger configured: 0x%02X", val);
    } else {
        ESP_LOGW(TAG, "Failed to communicate with AXP2101 (Reg 0x18)");
    }
    
    // 3. Enable All ADC Channels (Reg 0x30)
    // Writing 0x3F enables VBAT, VBUS, VSYS, TS, Die Temp, and Current ADCs.
    // The Fuel Gauge requires these to calculate Coulomb counting and temperature adjustments.
    if (pmu_write_reg(0x30, 0x3F) == ESP_OK) {
        ESP_LOGI(TAG, "All ADC Channels successfully enabled (Reg 0x30 = 0x3F)");
    } else {
        ESP_LOGW(TAG, "Failed to communicate with AXP2101 (Reg 0x30)");
    }

    while (1) {
        uint8_t status1 = 0;
        uint8_t status2 = 0;
        uint8_t percent = 0;
        uint8_t vbat_h = 0;
        uint8_t vbat_l = 0;
        
        // Read diagnostic status registers
        pmu_read_reg(0x00, &status1);
        pmu_read_reg(0x01, &status2);
        
        // Read Fuel Gauge Percentage (Reg 0xA4)
        esp_err_t ret_pct = pmu_read_reg(0xA4, &percent);
        
        // Read 14-Bit ADC Battery Voltage (Reg 0x34 & 0x35)
        esp_err_t ret_v_h = pmu_read_reg(0x34, &vbat_h);
        esp_err_t ret_v_l = pmu_read_reg(0x35, &vbat_l);
        
        bool present = (status1 & (1 << 3)) != 0;
        bool charging = (status2 & 0x07) == 1 || (status2 & 0x07) == 2 || (status2 & 0x07) == 3;
        
        if (ret_pct == ESP_OK && ret_v_h == ESP_OK && ret_v_l == ESP_OK) {
            // Reconstruct the 14-bit voltage (typically 1mV per step on standard AXP2101)
            uint16_t vbat_raw = ((vbat_h & 0x3F) << 8) | vbat_l;
            
            // Assign metrics to globally visible variables
            battery_percentage = (percent > 100) ? 100.0f : (float)percent;
            battery_voltage = (float)vbat_raw / 1000.0f;
            battery_present = present;
            battery_charging = charging;
            
            ESP_LOGI(TAG, "Battery metrics updated -> %d%% (%.3f V) [Present: %d, Charging: %d]", 
                     (int)battery_percentage, battery_voltage, battery_present, battery_charging);
        } else {
            ESP_LOGW(TAG, "Failed to read data registers over I2C!");
        }

        // Poll hardware values every 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    // Delay to let the serial terminal on the PC re-enumerate and connect safely before logs are sent
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Initializing Smartwatch OS display and peripherals...");

    // 1. Initialize Board Support Package Display and LVGL Graphics Engine
    bsp_display_start();
    
    // 2. Power on the display backlight panel
    bsp_display_backlight_on();

    // 3. Construct the clean battery UI dashboard
    build_ui();

    ESP_LOGI(TAG, "Display and UI initialized. Connecting to shared I2C bus...");

    // 4. Retrieve pre-initialized, shared BSP I2C Master Bus Handle
    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus == NULL) {
        bsp_i2c_init();
        bsp_bus = bsp_i2c_get_handle();
    }

    if (bsp_bus != NULL) {
        // Register PMU on the shared bus, avoiding conflicts with displays, RTC, and touch panels
        i2c_device_config_t pmu_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 100000,
        };
        
        esp_err_t ret = i2c_master_bus_add_device(bsp_bus, &pmu_cfg, &pmu_dev_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "AXP2101 registered successfully on shared BSP I2C bus");
        } else {
            ESP_LOGE(TAG, "Failed to register AXP2101 on shared bus: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Unable to obtain valid BSP I2C Bus handle!");
    }

    // 5. Create background FreeRTOS task to continuously poll battery
    xTaskCreate(battery_monitor_task, "battery_monitor_task", 4096, NULL, 5, NULL);
}
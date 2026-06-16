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

// Multi-byte read helper to prevent AXP2101 ADC buffers from resetting or tearing
esp_err_t pmu_read_regs(uint8_t reg_addr, uint8_t *data, size_t len) {
    if (pmu_dev_handle == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(pmu_dev_handle, &reg_addr, 1, data, len, 1000);
}

// Single-byte write helper
esp_err_t pmu_write_reg(uint8_t reg_addr, uint8_t data) {
    if (pmu_dev_handle == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg_addr, data};
    return i2c_master_transmit(pmu_dev_handle, buf, sizeof(buf), 1000);
}

void battery_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting battery monitor task...");

    // 1. Clear any pre-existing fault/IRQ flags that may be pausing the PMU
    pmu_write_reg(0x48, 0xFF);
    pmu_write_reg(0x49, 0xFF);
    pmu_write_reg(0x4A, 0xFF);
    pmu_write_reg(0x4B, 0xFF);
    pmu_write_reg(0x4C, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 2. Disable TS (Thermistor) Pin charging protection (Board doesn't have one)
    // REG 0x50: Bit 4 = 1 (External fixed input, doesn't affect charger)
    pmu_write_reg(0x50, 0x10); 
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 3. Force battery presence detection to "Always Present"
    // REG 0x68: Bit 0 = 0 (disable detection, PMU considers battery always present)
    pmu_write_reg(0x68, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 4. Enable Fuel Gauge and Charging circuits
    // REG 0x18: Bit 3 = Gauge enable, Bit 2 = Button charge, Bit 1 = Cell charge
    pmu_write_reg(0x18, 0x0E);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 5. Enable MASTER ADC Engine + VBAT & VBUS ADCs (Reg 0x30)
    // Bit 5 = Master ADC Enable, Bit 2 = VBUS ADC, Bit 0 = Battery ADC (0x25 = 0010 0101)
    pmu_write_reg(0x30, 0x25);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 6. Disable low frequency sampling mode to force fast ADC updates
    uint8_t ts_h = 0;
    if (pmu_read_regs(0x36, &ts_h, 1) == ESP_OK) {
        pmu_write_reg(0x36, ts_h & 0x7F);
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    while (1) {
        uint8_t status1 = 0;
        uint8_t percent = 0;
        uint8_t vbat_data[2] = {0, 0};
        
        // Read diagnostic status
        esp_err_t err = pmu_read_regs(0x00, &status1, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C read failed: %s. Retrying...", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Read Fuel Gauge Percentage (Reg 0xA4)
        pmu_read_regs(0xA4, &percent, 1);
        
        // Read 14-Bit ADC Battery Voltage (Reg 0x34 & 0x35) in a single multi-byte transaction
        pmu_read_regs(0x34, vbat_data, 2);
        
        // Reconstruct the 14-bit voltage (1mV per LSB step)
        uint16_t vbat_raw = ((vbat_data[0] & 0x3F) << 8) | vbat_data[1];
        float voltage = (float)vbat_raw / 1000.0f;
        
        // VBUS Good = Bit 5 of Status 1 (Is USB Power Connected?)
        bool vbus_good = (status1 & (1 << 5)) != 0;
        
        // Map to global UI variables
        battery_voltage = voltage;
        battery_percentage = (percent > 100) ? 100.0f : (float)percent;
        
        // Fallback: If the PMU fuel gauge hasn't calibrated yet but the voltage is high enough
        if (battery_percentage == 0.0f && battery_voltage > 3.0f) {
            battery_percentage = ((battery_voltage - 3.3f) / 0.9f) * 100.0f;
            if (battery_percentage > 100.0f) battery_percentage = 100.0f;
            if (battery_percentage < 0.0f) battery_percentage = 0.0f;
        }

        // Trust the raw ADC: If the voltage is above 2.0V, the battery is physically connected.
        battery_present = (status1 & (1 << 3)) != 0;
        if (!battery_present && battery_voltage > 2.0f) {
            battery_present = true;
        }
        
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
            // Lowered to 100kHz to ensure extreme stability with the PMU and prevent pull-up warnings
            .scl_speed_hz = 100000, 
        };
        
        esp_err_t ret = i2c_master_bus_add_device(bsp_bus, &pmu_cfg, &pmu_dev_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "AXP2101 registered successfully on shared BSP I2C bus");
        }
    }

    xTaskCreate(battery_monitor_task, "battery_monitor_task", 4096, NULL, 5, NULL);
}
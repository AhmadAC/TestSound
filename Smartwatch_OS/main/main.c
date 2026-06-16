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
        uint8_t reg18 = 0;
        uint8_t reg30 = 0;
        uint8_t reg68 = 0;
        uint8_t percent = 0;
        uint8_t vbat_h = 0;
        uint8_t vbat_l = 0;
        
        // Read diagnostic status registers
        pmu_read_reg(0x00, &status1);
        pmu_read_reg(0x01, &status2);
        pmu_read_reg(0x18, &reg18);
        pmu_read_reg(0x30, &reg30);
        pmu_read_reg(0x68, &reg68);
        
        // Read Fuel Gauge Percentage (Reg 0xA4)
        esp_err_t ret_pct = pmu_read_reg(0xA4, &percent);
        
        // Read 14-Bit ADC Battery Voltage (Reg 0x34 & 0x35)
        esp_err_t ret_v_h = pmu_read_reg(0x34, &vbat_h);
        esp_err_t ret_v_l = pmu_read_reg(0x35, &vbat_l);
        
        bool present = (status1 & (1 << 3)) != 0;
        bool charging = (status2 & 0x07) == 1 || (status2 & 0x07) == 2 || (status2 & 0x07) == 3;
        
        ESP_LOGI(TAG, "--- PMU DIAGNOSTIC DUMP ---");
        ESP_LOGI(TAG, "Reg 0x00 (Status1)  : 0x%02X [BatPresent=%d]", status1, present);
        ESP_LOGI(TAG, "Reg 0x01 (Status2)  : 0x%02X [Charging=%d]", status2, charging);
        ESP_LOGI(TAG, "Reg 0x18 (GaugeCtrl): 0x%02X", reg18);
        ESP_LOGI(TAG, "Reg 0x30 (ADCCtrl)  : 0x%02X", reg30);
        ESP_LOGI(TAG, "Reg 0x68 (BatDet)   : 0x%02X", reg68);
        ESP_LOGI(TAG, "Reg 0x34/35 (RawV)  : 0x%02X / 0x%02X", vbat_h, vbat_l);
        ESP_LOGI(TAG, "Reg 0xA4 (Percent)  : 0x%02X (%d%%)", percent, percent);

        if (ret_pct == ESP_OK && ret_v_h == ESP_OK && ret_v_l == ESP_OK) {
            // Reconstruct the 14-bit voltage (typically 1mV per step on standard AXP2101)
            uint16_t vbat_raw = ((vbat_h & 0x3F) << 8) | vbat_l;
            uint16_t vbat_mv = vbat_raw;
            
            // Safety cap percentage
            if (percent > 100) percent = 100;
            
            ESP_LOGI(TAG, "Calculated State -> Battery: %d%% (%d mV) [Raw: %d]", percent, vbat_mv, vbat_raw);
        } else {
            ESP_LOGW(TAG, "Failed to read data registers over I2C!");
        }

        // Poll battery metrics every 5 seconds
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
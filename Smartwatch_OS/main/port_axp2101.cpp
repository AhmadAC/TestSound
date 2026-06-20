// File: Smartwatch_OS/main/port_axp2101.cpp
#include <stdio.h>
#include <cstring>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"

// Include the XPowers library (Defaults to SY6970 on V2.0.0 hardware)
#include "XPowersLib.h"

static const char *TAG = "PMU";

static PowersSY6970 PMU_SY;
static XPowersAXP2101 PMU_AXP;
static bool is_sy6970 = false;

extern int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len);
extern int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len);

extern "C" {
    extern float battery_percentage;
    extern float battery_voltage;
    extern bool battery_present;
    extern bool battery_charging;
    extern uint8_t get_pmu_address();
}

esp_err_t pmu_init()
{
    uint8_t addr = get_pmu_address();
    if (addr == 0x6A) {
        is_sy6970 = true;
        // Address doesn't matter since pmu_register_read uses the active handle mapped in main
        if (PMU_SY.begin(0x6A, pmu_register_read, pmu_register_write_byte)) {
            ESP_LOGI(TAG, "SY6970 PMU initialized successfully!");
            PMU_SY.enableADCMeasure();
            PMU_SY.enableCharge();
            return ESP_OK;
        }
    } else if (addr == 0x34) {
        is_sy6970 = false;
        if (PMU_AXP.begin(0x34, pmu_register_read, pmu_register_write_byte)) {
            ESP_LOGI(TAG, "AXP2101 PMU initialized successfully!");
            
            // Explicitly enable Cell Battery Charger & E-Gauge (Fuel Gauge)
            PMU_AXP.writeRegister(XPOWERS_AXP2101_CHARGE_GAUGE_WDT_CTRL, 0x0A);
            
            // Enable all ADC channels (VBAT, VBUS, VSYS, TS, Tdie, and GPADC)
            PMU_AXP.writeRegister(XPOWERS_AXP2101_ADC_CHANNEL_CTRL, 0x3F);
            
            // Enable active hardware battery presence detection
            PMU_AXP.writeRegister(XPOWERS_AXP2101_BAT_DET_CTRL, 0x01);
            
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Failed to communicate with PMU!");
    return ESP_FAIL;
}

void pmu_isr_handler()
{
    if (is_sy6970) {
        battery_voltage = PMU_SY.getBattVoltage() / 1000.0f;
        battery_charging = PMU_SY.isVbusIn();
    } else {
        uint16_t vbat = PMU_AXP.getBattVoltage();
        if (vbat == 0) {
            // Bypass isBatteryConnect() check and read the raw VBAT ADC registers directly
            vbat = PMU_AXP.readRegisterH5L8(XPOWERS_AXP2101_ADC_DATA_RELUST0, XPOWERS_AXP2101_ADC_DATA_RELUST1);
        }
        battery_voltage = vbat / 1000.0f;
        
        // Use the highly reliable isVbusGood() status for charging / USB connection
        battery_charging = PMU_AXP.isVbusGood();
    }
    
    // Determine if battery is present based on measured voltage
    battery_present = (battery_voltage > 2.0f);

    if (battery_present) {
        if (!is_sy6970) {
            // Read AXP2101's internal fuel gauge percentage directly (bypassing library isBatteryConnect checks)
            int pct = PMU_AXP.readRegister(XPOWERS_AXP2101_BAT_PERCENT_DATA);
            if (pct >= 0 && pct <= 100) {
                battery_percentage = (float)pct;
            } else {
                // Fallback to LiPo Curve
                if (battery_voltage >= 4.15f) {
                    battery_percentage = 100.0f;
                } else if (battery_voltage <= 3.3f) {
                    battery_percentage = 0.0f;
                } else {
                    battery_percentage = ((battery_voltage - 3.3f) / 0.85f) * 100.0f;
                }
            }
        } else {
            // SY6970 LiPo Curve
            if (battery_voltage >= 4.15f) {
                battery_percentage = 100.0f;
            } else if (battery_voltage <= 3.3f) {
                battery_percentage = 0.0f;
            } else {
                battery_percentage = ((battery_voltage - 3.3f) / 0.85f) * 100.0f;
            }
        }
    } else {
        battery_percentage = 0.0f;
    }

    ESP_LOGI(TAG, "Battery metrics: %.3f V | %d%% [Charging: %d, Present: %d]", 
             battery_voltage, (int)battery_percentage, battery_charging, battery_present);
}
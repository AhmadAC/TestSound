// File: Smartwatch_OS/main/port_axp2101.cpp
#include <stdio.h>
#include <cstring>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"

// Include the XPowers library (Defaults to SY6970 on V2.0.0 hardware)
#include "XPowersLib.h"

static const char *TAG = "SY6970";

static PowersSY6970 PMU;

extern int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len);
extern int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len);

extern "C" {
    extern float battery_percentage;
    extern float battery_voltage;
    extern bool battery_present;
    extern bool battery_charging;
}

esp_err_t pmu_init()
{
    // Use 0x6B as established from your address scan
    if (PMU.begin(0x6B, pmu_register_read, pmu_register_write_byte)) {
        ESP_LOGI(TAG, "SY6970 PMU initialized successfully at 0x6B!");
    } else {
        ESP_LOGE(TAG, "Failed to communicate with SY6970 PMU!");
        return ESP_FAIL;
    }

    // Enable ADCs to measure Battery Voltage and USB Voltage
    PMU.enableADCMeasure();

    // Enable charging circuit
    PMU.enableCharge();

    return ESP_OK;
}

void pmu_isr_handler()
{
    // Retrieve battery voltage from SY6970 ADC (returns in mV)
    battery_voltage = PMU.getBattVoltage() / 1000.0f;
    
    // The SY6970 is a charger, not a fuel gauge, so we estimate percentage based on LiPo curve
    if (battery_voltage >= 4.15f) {
        battery_percentage = 100.0f;
    } else if (battery_voltage <= 3.3f) {
        battery_percentage = 0.0f;
    } else {
        battery_percentage = ((battery_voltage - 3.3f) / 0.85f) * 100.0f;
    }

    // Check if USB is supplying power
    battery_charging = PMU.isVbusIn();

    // Simple heuristic for presence: if voltage is very low, the battery is missing
    battery_present = (battery_voltage > 2.5f);

    ESP_LOGI(TAG, "Battery metrics: %.3f V | %d%% [Charging: %d, VBUS: %d]", 
             battery_voltage, (int)battery_percentage, battery_charging, PMU.isVbusIn());
}
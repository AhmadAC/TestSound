// File: Smartwatch_OS/main/port_axp2101.cpp
#include <stdio.h>
#include <cstring>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
static const char *TAG = "AXP2101";

static XPowersPMU PMU;

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
    if (PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
        ESP_LOGI(TAG, "XPowersLib initialized PMU successfully!");
    } else {
        ESP_LOGE(TAG, "XPowersLib failed to communicate with PMU!");
        return ESP_FAIL;
    }

    // CRITICAL FIX: Safely enable all measurement ADCs without overwriting other bits
    PMU.enableBattVoltageMeasure();
    PMU.enableVbusVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();

    // CRITICAL FIX: Enable Battery Detection
    PMU.enableBattDetection();

    // CRITICAL FIX: Enable Fuel Gauge AND Cell Battery Charging!
    // Without cell charging enabled, a completely dead battery's BMS will never wake up 
    // and will permanently read 0.000 V.
    PMU.enableGauge();
    PMU.enableCellbatteryCharge();

    // Standard charging specifications
    PMU.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    PMU.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    // Disable all PMU interrupts to prevent unexpected behaviors
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    PMU.clearIrqStatus();

    return ESP_OK;
}

void pmu_isr_handler()
{
    // Read latest status mask
    PMU.getIrqStatus();

    // The library safely reads the 14-bit ADC registers
    battery_voltage = PMU.getBattVoltage() / 1000.0f;
    battery_percentage = (float)PMU.getBatteryPercent();

    // Safely fallback to raw ADC reads if the library blocks getBattVoltage due to detection lag
    if (battery_voltage == 0.0f) {
        uint8_t v_high = 0, v_low = 0;
        if (pmu_register_read(0x34, 0x34, &v_high, 1) == 0 && pmu_register_read(0x34, 0x35, &v_low, 1) == 0) {
            uint16_t raw_mv = ((v_high & 0x1F) << 8) | v_low;
            battery_voltage = (float)raw_mv / 1000.0f;
        }
    }

    // Read Connection States
    battery_present = PMU.isBatteryConnect();
    battery_charging = PMU.isVbusGood();

    // Force physical state assumptions if hardware registers are lagging
    if (!battery_present && battery_voltage > 2.0f) {
        battery_present = true;
    }
    if (!battery_charging && battery_voltage > 4.15f) {
        battery_charging = true;
    }

    ESP_LOGI(TAG, "Battery metrics: %.3f V | %d%% [Connected: %d, USB: %d]", 
             battery_voltage, (int)battery_percentage, battery_present, battery_charging);

    // Acknowledge and clear PMU event states
    PMU.clearIrqStatus();
}
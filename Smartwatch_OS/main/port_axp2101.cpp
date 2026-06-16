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
    if (PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte))
    {
        ESP_LOGI(TAG, "XPowersLib initialized PMU successfully!");
    }
    else
    {
        ESP_LOGE(TAG, "XPowersLib failed to communicate with PMU!");
        return ESP_FAIL;
    }

    // Disable unused channels to conserve power
    PMU.disableDC2();
    PMU.disableDC3();
    PMU.disableDC4();
    PMU.disableDC5();

    PMU.disableALDO1();
    PMU.disableALDO2();
    PMU.disableALDO3();
    PMU.disableALDO4();
    PMU.disableBLDO1();
    PMU.disableBLDO2();

    PMU.disableCPUSLDO();
    PMU.disableDLDO1();
    PMU.disableDLDO2();

    // Ensure DC1 (powers ESP32 core and system logic) is enabled at 3.3V
    PMU.setDC1Voltage(3300);
    PMU.enableDC1();

    // Ensure ALDO1 (powers AMOLED backlight panel) is enabled at 3.3V
    PMU.setALDO1Voltage(3300);
    PMU.enableALDO1();

    PMU.clearIrqStatus();

    // Enable PMU ADC measurements
    PMU.enableVbusVoltageMeasure();
    PMU.enableBattVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();

    // Completely disable physical TS pin measurement to prevent faults (no thermistor on board)
    PMU.disableTSPinMeasure();

    // Disable all PMU interrupts and clear status registers
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    PMU.clearIrqStatus();

    // Enable required PMU system events
    PMU.enableIRQ(
        XPOWERS_AXP2101_BAT_INSERT_IRQ | XPOWERS_AXP2101_BAT_REMOVE_IRQ |    
        XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ |  
        XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ |     
        XPOWERS_AXP2101_BAT_CHG_DONE_IRQ | XPOWERS_AXP2101_BAT_CHG_START_IRQ 
    );

    // Standard charging specifications
    PMU.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    PMU.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    return ESP_OK;
}

void pmu_isr_handler()
{
    // Read latest status mask
    PMU.getIrqStatus();

    // Collect and update global parameters with error-safe defaults
    battery_voltage = (float)PMU.getBattVoltage() / 1000.0f;
    
    int percent = PMU.getBatteryPercent();
    battery_percentage = (percent < 0) ? 0.0f : (float)percent;
    
    battery_present = PMU.isBatteryConnect();
    battery_charging = PMU.isVbusIn();

    // Fallback percentage calculation based on voltage curves if gauge hasn't converged
    if (battery_percentage == 0.0f && battery_voltage > 3.0f) {
        battery_percentage = ((battery_voltage - 3.3f) / 0.9f) * 100.0f;
        if (battery_percentage > 100.0f) battery_percentage = 100.0f;
        if (battery_percentage < 0.0f) battery_percentage = 0.0f;
    }

    // Trust voltage readings for physical battery connections
    if (!battery_present && battery_voltage > 2.0f) {
        battery_present = true;
    }

    ESP_LOGI(TAG, "Battery metrics: %.3f V | %d%% [Connected: %d, USB: %d]", 
             battery_voltage, (int)battery_percentage, battery_present, battery_charging);

    // Acknowledge and clear PMU event states
    PMU.clearIrqStatus();
}
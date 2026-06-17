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

#define I2C_MASTER_TIMEOUT_MS 1000

// Confirmed hardware I2C pins for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch
#define I2C_PMU_SDA_IO     15
#define I2C_PMU_SCL_IO     14

static i2c_master_dev_handle_t pmu_dev_handle = NULL;
static i2c_master_bus_handle_t pmu_bus_handle = NULL;

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
void i2c_clear_bus(int sda, int scl) {
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
    gpio_set_direction((gpio_num_t)sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)scl, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);
    
    gpio_set_level((gpio_num_t)sda, 1);
    gpio_set_level((gpio_num_t)scl, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Toggle SCL 9 times to free any stuck slaves
    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)scl, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level((gpio_num_t)scl, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Verifies device is a real PMU by reading its physical Chip ID register (0x03)
esp_err_t verify_pmu(i2c_master_dev_handle_t dev, int sda, int scl, uint8_t *out_id) {
    uint8_t reg = 0x03;
    uint8_t chip_id = 0;
    
    // Perform transaction
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, &chip_id, 1, 100);
    if (ret == ESP_OK) {
        *out_id = chip_id;
        // 0x4A is AXP2101, 0x41 is AXP202, 0x03 is AXP192
        if (chip_id == 0x4A || chip_id == 0x41 || chip_id == 0x03) {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

// Brute-force scanner that tests safe GPIO pins on I2C Port 0
esp_err_t brute_force_pmu_scan(int *out_sda, int *out_scl) {
    // List of safe pins, prioritized with 15 (SDA) and 14 (SCL) at the very front
    int safe_pins[] = {15, 14, 6, 7, 8, 9, 4, 5, 10, 11, 12, 13, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48};
    int num_pins = sizeof(safe_pins) / sizeof(safe_pins[0]);

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, " RUNNING DIAGNOSTIC BRUTE-FORCE I2C SCANNER...   ");
    ESP_LOGW(TAG, "==================================================");

    for (int sda_idx = 0; sda_idx < num_pins; sda_idx++) {
        for (int scl_idx = 0; scl_idx < num_pins; scl_idx++) {
            if (sda_idx == scl_idx) continue;

            int sda = safe_pins[sda_idx];
            int scl = safe_pins[scl_idx];

            i2c_clear_bus(sda, scl);

            i2c_master_bus_config_t i2c_mst_config = {};
            i2c_mst_config.i2c_port = I2C_NUM_0; // Explicitly lock to Port 0 to override BSP later
            i2c_mst_config.sda_io_num = (gpio_num_t)sda;
            i2c_mst_config.scl_io_num = (gpio_num_t)scl;
            i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
            i2c_mst_config.glitch_ignore_cnt = 7;
            i2c_mst_config.flags.enable_internal_pullup = 1; // Enable internal pull-ups

            i2c_master_bus_handle_t temp_bus;
            if (i2c_new_master_bus(&i2c_mst_config, &temp_bus) != ESP_OK) {
                continue;
            }

            i2c_device_config_t dev_config = {};
            dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_config.device_address = 0x34; // AXP2101 Address
            dev_config.scl_speed_hz = 100000;  // 100 kHz is required for internal pullups to work!

            i2c_master_dev_handle_t temp_dev;
            if (i2c_master_bus_add_device(temp_bus, &dev_config, &temp_dev) != ESP_OK) {
                i2c_del_master_bus(temp_bus);
                continue;
            }

            uint8_t read_id = 0;
            esp_err_t verify_ret = verify_pmu(temp_dev, sda, scl, &read_id);
            
            // Print the scan result for EVERY single pin pair tested
            ESP_LOGI(TAG, "Testing SDA=%d, SCL=%d | Read ID: 0x%02X | Result: %s", 
                     sda, scl, read_id, (verify_ret == ESP_OK) ? "SUCCESS!" : "FAIL");

            if (verify_ret == ESP_OK) {
                ESP_LOGW(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                ESP_LOGW(TAG, "!! PMU VERIFIED ON SDA=%d, SCL=%d (ID: 0x%02X) !!", sda, scl, read_id);
                ESP_LOGW(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                *out_sda = sda;
                *out_scl = scl;
                
                pmu_dev_handle = temp_dev;
                pmu_bus_handle = temp_bus;
                return ESP_OK;
            }

            // Cleanup if not verified
            i2c_master_bus_rm_device(temp_dev);
            i2c_del_master_bus(temp_bus);
        }
    }

    return ESP_ERR_NOT_FOUND;
}

// PMU read function utilizing the active I2C handle
int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    esp_err_t ret = i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Read Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

// PMU write function utilizing the active I2C handle
int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    if (pmu_dev_handle == NULL) return -1;
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU I2C Write Reg 0x%02X Failed! Err: %s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

// Background task to poll the battery metrics
static void pmu_hander_task(void *args) {
    while (1) {
        pmu_isr_handler();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup_pmu_i2c() {
    // 1. First, check if the PMU is just sharing the display's I2C bus (Very common)
    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (bsp_bus != NULL) {
        ESP_LOGI(TAG, "Checking if PMU is on the BSP's shared display I2C bus...");
        if (i2c_master_probe(bsp_bus, 0x34, 100) == ESP_OK) {
            ESP_LOGI(TAG, "--> YES! PMU found on the BSP shared I2C bus.");
            i2c_device_config_t dev_config = {};
            dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_config.device_address = 0x34;
            dev_config.scl_speed_hz = 100000;
            dev_config.flags.disable_ack_check = 0;
            
            esp_err_t ret = i2c_master_bus_add_device(bsp_bus, &dev_config, &pmu_dev_handle);
            if (ret == ESP_OK) {
                // Initialize the PMU on the working bus
                if (pmu_init() == ESP_OK) {
                    xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
                    ESP_LOGI(TAG, "PMU registered and initialized permanently on the shared I2C bus.");
                    return;
                } else {
                    ESP_LOGE(TAG, "PMU init failed on the shared BSP bus.");
                    // Clean up and fallback to scan
                    i2c_master_bus_rm_device(pmu_dev_handle);
                    pmu_dev_handle = NULL;
                }
            }
        } else {
            ESP_LOGI(TAG, "--> NO. PMU is not on the BSP bus.");
        }
    }

    // 2. If not on the shared bus, brute-force scan all safe pins!
    int sda_found = -1;
    int scl_found = -1;
    if (brute_force_pmu_scan(&sda_found, &scl_found) == ESP_OK) {
        ESP_LOGI(TAG, "PMU found on scanned pins SDA=%d, SCL=%d", sda_found, scl_found);
        if (pmu_init() == ESP_OK) {
            xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
            ESP_LOGI(TAG, "PMU successfully initialized on scanned bus.");
        } else {
            ESP_LOGE(TAG, "PMU init failed on scanned bus.");
        }
    } else {
        ESP_LOGE(TAG, "PMU not found on any pins!");
    }
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Starting Smartwatch OS...");

    // 1. Initialize display and UI FIRST. This ensures the BSP configures the shared I2C bus.
    bsp_display_start();
    bsp_display_backlight_on();
    build_ui();

    // 2. FORCE internal pull-ups on GPIO 15 and 14 after the BSP display driver overrides them!
    gpio_set_pull_mode((gpio_num_t)15, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)14, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow lines to rise to VCC

    // 3. Locate, register, and initialize PMU
    setup_pmu_i2c();
}
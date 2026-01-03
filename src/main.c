/**
 * @file main.c
 * @brief CO2 Sensor Display - Main Application
 *
 * ESP32-C3 based CO2 sensor display using 4.2" e-paper.
 * Displays readings from 4 Inkbird IAM-T1 CO2 sensors via BLE.
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"

#include "epd_driver.h"
#include "sensor_data.h"
#include "synthetic_data.h"
#include "ui_co2_display.h"
#include "inkbird_ble.h"

static const char *TAG = "main";

// Set to true to use synthetic data (for testing without sensors)
// Set to false to use real BLE sensors
#define USE_SYNTHETIC_DATA  false

// Update intervals
#define SENSOR_UPDATE_MS        60000   // Sensor data update (1 minute)
#define DISPLAY_REFRESH_MS      60000   // E-paper refresh interval (1 minute)

// FreeRTOS timer handles
static TimerHandle_t s_sensor_timer = NULL;
static TimerHandle_t s_refresh_timer = NULL;

// Flag to trigger display refresh
static volatile bool s_do_refresh = false;

/**
 * @brief Sensor data update timer callback
 *
 * In BLE mode: copies readings from BLE module to sensor_data
 * In synthetic mode: generates fake data for testing
 */
static void sensor_update_cb(TimerHandle_t timer)
{
    (void)timer;

#if USE_SYNTHETIC_DATA
    // Generate new synthetic data for testing
    synthetic_data_update();
    ESP_LOGI(TAG, "Sensor data updated (synthetic, step %lu)", 
             (unsigned long)synthetic_data_get_step());
#else
    // Copy readings from BLE module to sensor_data
    for (int i = 0; i < SENSOR_COUNT; i++) {
        inkbird_reading_t ble_reading = inkbird_ble_get_reading(i);
        
        if (ble_reading.valid) {
            sensor_reading_t reading = {
                .co2_ppm = ble_reading.co2_ppm,
                .temperature = ble_reading.temperature,
                .humidity = ble_reading.humidity,
                .pressure = ble_reading.pressure,
                .timestamp = ble_reading.timestamp,
            };
            sensor_data_update(i, &reading);
            
            // Update connected status based on staleness
            sensor_data_t *sensor = sensor_data_get(i);
            if (sensor) {
                sensor->connected = !ble_reading.stale;
            }
        }
    }
    ESP_LOGI(TAG, "Sensor data updated from BLE");
#endif
}

/**
 * @brief Display refresh timer callback
 */
static void display_refresh_cb(TimerHandle_t timer)
{
    (void)timer;
    s_do_refresh = true;
}

/**
 * @brief Display task - handles rendering and display updates
 */
static void display_task(void *arg)
{
    (void)arg;
    
    ESP_LOGI(TAG, "Display task started");
    
    // Initial render and refresh
    vTaskDelay(pdMS_TO_TICKS(100));
    ui_co2_display_update();
    epd_refresh();
    
    while (1) {
        if (s_do_refresh) {
            s_do_refresh = false;
            
            // Re-render UI and refresh display
            ui_co2_display_update();
            epd_refresh();
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  CO2 Sensor Display");
    ESP_LOGI(TAG, "  ESP32-C3 + E-Paper + Inkbird BLE");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
    
    // Initialize e-paper display hardware
    ESP_LOGI(TAG, "Initializing e-paper display...");
    esp_err_t ret = epd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "E-paper init failed!");
        return;
    }
    
    // Initialize sensor data module
    ESP_LOGI(TAG, "Initializing sensor data...");
    sensor_data_init();

    // Set sensor names from BLE configuration
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (inkbird_ble_is_sensor_enabled(i)) {
            sensor_data_set_name(i, inkbird_ble_get_sensor_name(i));
        }
    }

#if USE_SYNTHETIC_DATA
    // Initialize synthetic data generator for testing
    ESP_LOGI(TAG, "Using SYNTHETIC data (no BLE)");
    synthetic_data_init();
    synthetic_data_prefill_history(SENSOR_HISTORY_SIZE);
#else
    // Initialize BLE for Inkbird sensors
    ESP_LOGI(TAG, "Initializing Bluetooth for Inkbird sensors...");
    ret = inkbird_ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed! Falling back to synthetic data.");
        synthetic_data_init();
        synthetic_data_prefill_history(SENSOR_HISTORY_SIZE);
    } else {
        // Run discovery to find sensors (comment out after MACs are configured)
        ESP_LOGI(TAG, "Running sensor discovery...");
        inkbird_ble_discover();
        
        // Start BLE reading cycle
        ESP_LOGI(TAG, "Starting BLE sensor reading...");
        inkbird_ble_start();
    }
#endif
    
    // Initialize UI
    ESP_LOGI(TAG, "Initializing UI...");
    ui_co2_display_init();
    
    // Create sensor update timer
    s_sensor_timer = xTimerCreate(
        "sensor_update",
        pdMS_TO_TICKS(SENSOR_UPDATE_MS),
        pdTRUE,  // Auto-reload
        NULL,
        sensor_update_cb
    );
    xTimerStart(s_sensor_timer, 0);
    
    // Create display refresh timer
    s_refresh_timer = xTimerCreate(
        "display_refresh",
        pdMS_TO_TICKS(DISPLAY_REFRESH_MS),
        pdTRUE,  // Auto-reload
        NULL,
        display_refresh_cb
    );
    xTimerStart(s_refresh_timer, 0);
    
    // Create display task
    ESP_LOGI(TAG, "Starting display task...");
    xTaskCreate(
        display_task,
        "display",
        4096,    // Stack size
        NULL,
        5,       // Priority
        NULL
    );
    
    ESP_LOGI(TAG, "Initialization complete!");
    ESP_LOGI(TAG, "Display will refresh every %d seconds", DISPLAY_REFRESH_MS / 1000);
    
    // Main task can sleep
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

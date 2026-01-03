/**
 * @file main.c
 * @brief CO2 Sensor Display - Main Application
 *
 * ESP32-C3 based CO2 sensor display using 4.2" e-paper.
 * Displays readings from 4 CO2 sensors with historical charts.
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

static const char *TAG = "main";

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
 */
static void sensor_update_cb(TimerHandle_t timer)
{
    (void)timer;
    
    // Generate new synthetic data
    synthetic_data_update();
    
    ESP_LOGI(TAG, "Sensor data updated (step %lu)", 
             (unsigned long)synthetic_data_get_step());
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
    ESP_LOGI(TAG, "  ESP32-C3 + E-Paper");
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
    
    // Initialize synthetic data generator and pre-fill history
    ESP_LOGI(TAG, "Generating initial sensor data...");
    synthetic_data_init();
    synthetic_data_prefill_history(SENSOR_HISTORY_SIZE);
    
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

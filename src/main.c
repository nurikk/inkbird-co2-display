/**
 * @file main.c
 * @brief CO2 Sensor Display - Main Application
 *
 * ESP32-C3 based CO2 sensor display using LVGL on 4.2" e-paper.
 * Displays readings from 4 CO2 sensors with historical charts.
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "epd_driver.h"
#include "sensor_data.h"
#include "synthetic_data.h"
#include "ui_co2_display.h"

static const char *TAG = "main";

// Update intervals
#define LVGL_TICK_PERIOD_MS     10      // LVGL tick period
#define SENSOR_UPDATE_MS        60000   // Sensor data update (1 minute)
#define DISPLAY_REFRESH_MS      60000   // E-paper refresh interval (1 minute)

// FreeRTOS timer handles
static TimerHandle_t s_lvgl_tick_timer = NULL;
static TimerHandle_t s_sensor_timer = NULL;
static TimerHandle_t s_refresh_timer = NULL;

// Flag to trigger display refresh
static volatile bool s_do_refresh = false;

/**
 * @brief LVGL tick timer callback
 */
static void lvgl_tick_cb(TimerHandle_t timer)
{
    (void)timer;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/**
 * @brief Sensor data update timer callback
 */
static void sensor_update_cb(TimerHandle_t timer)
{
    (void)timer;
    
    // Generate new synthetic data
    synthetic_data_update();
    
    // Update UI with new data
    ui_co2_display_update();
    
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
 * @brief LVGL task - handles rendering and display updates
 */
static void lvgl_task(void *arg)
{
    (void)arg;
    
    ESP_LOGI(TAG, "LVGL task started");
    
    // Initial display refresh
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Let LVGL render the initial frame
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(100));
    lv_timer_handler();
    
    // Refresh e-paper display
    ESP_LOGI(TAG, "Initial display refresh...");
    epd_refresh();
    
    while (1) {
        // Process LVGL tasks
        uint32_t delay_ms = lv_timer_handler();
        
        // Check if we need to refresh the e-paper
        if (s_do_refresh) {
            s_do_refresh = false;
            
            // Re-render and refresh display
            lv_timer_handler();
            epd_refresh();
        }
        
        // Clamp delay to reasonable range
        if (delay_ms < 5) delay_ms = 5;
        if (delay_ms > 100) delay_ms = 100;
        
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
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
    ESP_LOGI(TAG, "  ESP32-C3 + LVGL + E-Paper");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
    
    // Initialize e-paper display hardware
    ESP_LOGI(TAG, "Initializing e-paper display...");
    esp_err_t ret = epd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "E-paper init failed!");
        return;
    }
    
    // Initialize LVGL
    ESP_LOGI(TAG, "Initializing LVGL...");
    lv_init();
    
    // Initialize LVGL display driver
    ret = epd_lvgl_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL display driver init failed!");
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
    ESP_LOGI(TAG, "Creating UI...");
    ui_co2_display_init();
    ui_co2_display_update();
    
    // Create LVGL tick timer
    s_lvgl_tick_timer = xTimerCreate(
        "lvgl_tick",
        pdMS_TO_TICKS(LVGL_TICK_PERIOD_MS),
        pdTRUE,  // Auto-reload
        NULL,
        lvgl_tick_cb
    );
    xTimerStart(s_lvgl_tick_timer, 0);
    
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
    
    // Create LVGL task
    ESP_LOGI(TAG, "Starting LVGL task...");
    xTaskCreate(
        lvgl_task,
        "lvgl",
        8192,    // Stack size
        NULL,
        5,       // Priority
        NULL
    );
    
    ESP_LOGI(TAG, "Initialization complete!");
    ESP_LOGI(TAG, "Display will refresh every %d seconds", DISPLAY_REFRESH_MS / 1000);
    
    // Main task can sleep - everything runs in timers and tasks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

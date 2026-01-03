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
#include "ui_co2_display.h"
#include "inkbird_ble.h"

static const char *TAG = "main";

// Update intervals
#define SENSOR_UPDATE_MS        60000   // Sensor data update (1 minute)
#define DISPLAY_REFRESH_MS      60000   // E-paper refresh interval (1 minute)

// History download configuration
// Use SENSOR_HISTORY_SIZE so we download exactly what the chart can display
#define DOWNLOAD_HISTORY_ON_STARTUP  true  // Set to false to skip history download

// FreeRTOS timer handles
static TimerHandle_t s_sensor_timer = NULL;
static TimerHandle_t s_refresh_timer = NULL;

// Flag to trigger display refresh
static volatile bool s_do_refresh = false;

// History storage (static allocation) - use SENSOR_HISTORY_SIZE to match chart capacity
static inkbird_history_record_t s_history_records[SENSOR_HISTORY_SIZE];

/**
 * @brief Download historical data from a sensor
 */
static void download_sensor_history(uint8_t sensor_idx)
{
    if (!inkbird_ble_is_sensor_enabled(sensor_idx)) {
        return;
    }
    
    sensor_data_t *sensor = sensor_data_get(sensor_idx);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Downloading History from Sensor %d", sensor_idx);
    ESP_LOGI(TAG, "  Name: %s", inkbird_ble_get_sensor_name(sensor_idx));
    ESP_LOGI(TAG, "========================================");

    uint16_t count = 0;
    esp_err_t ret = inkbird_ble_download_history(
        sensor_idx,
        s_history_records,
        SENSOR_HISTORY_SIZE,
        &count
    );
    
    // Clear downloading flag
    if (sensor) {
        sensor->downloading = false;
    }

    if (ret == ESP_OK && count > 0) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, ">>> History download SUCCESS: %u records <<<", count);
        
        // Feed history data into sensor_data module for chart display
        ESP_LOGI(TAG, "Adding %u records to chart", count);
        
        // Log first and last records
        ESP_LOGI(TAG, "  First: CO2=%u ppm, T=%.1f C",
                 s_history_records[0].co2_ppm,
                 s_history_records[0].temperature / 10.0f);
        if (count > 1) {
            ESP_LOGI(TAG, "  Last:  CO2=%u ppm, T=%.1f C",
                     s_history_records[count-1].co2_ppm,
                     s_history_records[count-1].temperature / 10.0f);
        }
        
        // Feed all records to chart history only (don't update current reading)
        // Records are already in chronological order (oldest first)
        for (int i = 0; i < count; i++) {
            inkbird_history_record_t *hist = &s_history_records[i];
            // Only add valid CO2 readings to history
            if (hist->co2_ppm > 0 && hist->co2_ppm < 10000) {
                sensor_data_add_history(sensor_idx, hist->co2_ppm);
            }
        }
        
        // Mark sensor as connected since we got data
        if (sensor) {
            sensor->connected = true;
        }
    } else {
        ESP_LOGW(TAG, ">>> History download FAILED: %s <<<", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
}

/**
 * @brief Sensor data update timer callback
 *
 * Copies readings from BLE module to sensor_data.
 */
static void sensor_update_cb(TimerHandle_t timer)
{
    (void)timer;

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
 * @brief Read initial sensor value with retries
 *
 * Used during startup to get current sensor readings before the
 * periodic polling task begins.
 *
 * @param sensor_idx Sensor index
 * @return true if successfully read, false otherwise
 */
static bool read_initial_sensor_value(int sensor_idx)
{
    for (int attempt = 1; attempt <= INKBIRD_STARTUP_READ_RETRIES; attempt++) {
        ESP_LOGI(TAG, "Reading sensor %d (%s) - attempt %d/%d",
                 sensor_idx, inkbird_ble_get_sensor_name(sensor_idx),
                 attempt, INKBIRD_STARTUP_READ_RETRIES);

        inkbird_reading_t reading;
        esp_err_t ret = inkbird_ble_read_sensor_once(
            sensor_idx,
            INKBIRD_STARTUP_READ_TIMEOUT_MS,
            &reading);

        if (ret == ESP_OK && reading.valid) {
            // Copy to sensor_data module using sensor_reading_t
            sensor_reading_t sensor_reading = {
                .co2_ppm = reading.co2_ppm,
                .temperature = reading.temperature,
                .humidity = reading.humidity,
                .pressure = reading.pressure,
                .timestamp = reading.timestamp,
            };
            sensor_data_update(sensor_idx, &sensor_reading);

            // Mark as connected
            sensor_data_t *sensor = sensor_data_get(sensor_idx);
            if (sensor) {
                sensor->connected = true;
            }

            ESP_LOGI(TAG, "Sensor %d: CO2=%d ppm, Temp=%.1f C, Humidity=%.1f%%",
                     sensor_idx, reading.co2_ppm,
                     reading.temperature / 10.0f,
                     reading.humidity / 10.0f);
            return true;
        }

        ESP_LOGW(TAG, "Sensor %d read failed (attempt %d): %s",
                 sensor_idx, attempt, esp_err_to_name(ret));

        if (attempt < INKBIRD_STARTUP_READ_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(2000));  // Wait before retry
        }
    }

    ESP_LOGE(TAG, "Sensor %d: All %d read attempts failed",
             sensor_idx, INKBIRD_STARTUP_READ_RETRIES);
    return false;
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

    // Initialize BLE for Inkbird sensors
    ESP_LOGI(TAG, "Initializing Bluetooth for Inkbird sensors...");
    ret = inkbird_ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed!");
        return;
    }

    // Initialize UI
    ESP_LOGI(TAG, "Initializing UI...");
    ui_co2_display_init();

    // Show loading screen immediately
    ESP_LOGI(TAG, "Showing loading screen...");
    ui_co2_display_loading();
    epd_refresh();

    // ========== PHASE 1: Read current real-time values ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Phase 1: Reading Current Sensor Values");
    ESP_LOGI(TAG, "========================================");
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (inkbird_ble_is_sensor_enabled(i)) {
            read_initial_sensor_value(i);
            vTaskDelay(pdMS_TO_TICKS(1000));  // Brief delay between sensors
        }
    }

    // ========== PHASE 2: Render display with current values ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Phase 2: Displaying Current Values");
    ESP_LOGI(TAG, "========================================");
    ui_co2_display_update();
    epd_refresh();

#if DOWNLOAD_HISTORY_ON_STARTUP
    // ========== PHASE 3: Download historical data ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Phase 3: Syncing Historical Data");
    ESP_LOGI(TAG, "========================================");

    // Wait for any pending BLE operations to complete before starting history download
    // This ensures the connection from Phase 1 is fully closed
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Mark sensors as downloading (for internal state tracking)
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (inkbird_ble_is_sensor_enabled(i)) {
            sensor_data_t *sensor = sensor_data_get(i);
            if (sensor) {
                sensor->downloading = true;
            }
        }
    }

    // Download history from each sensor
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (inkbird_ble_is_sensor_enabled(i)) {
            download_sensor_history(i);
            vTaskDelay(pdMS_TO_TICKS(2000));  // Delay between sensors
        }
    }

    // Final display refresh with history charts
    ESP_LOGI(TAG, "Refreshing display with history charts...");
    ui_co2_display_update();
    epd_refresh();
#endif

    // ========== PHASE 4: Start periodic BLE reading ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Phase 4: Starting Periodic Updates");
    ESP_LOGI(TAG, "========================================");
    inkbird_ble_start();
    
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

/**
 * @file main.c
 * @brief CO2 Sensor Display - Main Application
 *
 * ESP32 based CO2 sensor display using 2.4" TFT.
 * Displays readings from 4 Inkbird IAM-T1 CO2 sensors via BLE.
 *
 * Boot sequence:
 * 1. Initialize display and show loading screen
 * 2. Show empty main screen (sensors in "Waiting..." state)
 * 3. Initialize BLE stack
 * 4. Discover sensors and update names
 * 5. Connect to each sensor, request data + settings
 * 6. Download historical data (optional)
 * 7. Start periodic sensor polling
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

#include "nvs_flash.h"

#include "epd_driver.h"
#include "sensor_data.h"
#include "detail_history.h"
#include "ui_co2_display.h"
#include "inkbird_ble.h"
#include "led_control.h"

static const char *TAG = "main";

// Heap monitoring macro
#define LOG_HEAP(label) do { \
    ESP_LOGI(TAG, "HEAP [%s]: free=%lu, largest=%lu", label, \
             (unsigned long)esp_get_free_heap_size(), \
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); \
} while(0)

// Update intervals
#define SENSOR_UPDATE_MS        60000   // Sensor data update (1 minute)
#define DISPLAY_REFRESH_MS      60000   // TFT refresh interval (1 minute)
#define PROGRESS_UPDATE_MS      200     // Progress bar update interval during sync
#define SENSOR_READ_RETRY_DELAY_MS 2000 // Delay between sensor read retries

// Display task configuration
#define DISPLAY_TASK_STACK_SIZE 8192
#define DISPLAY_TASK_PRIORITY   6
#define UPDATE_INTERVAL_FAST_US 100000  // 100ms when downloading
#define UPDATE_INTERVAL_NORMAL_US 500000 // 500ms normal operation
#define TOUCH_STATUS_LOG_INTERVAL_US 60000000 // 60s between touch status logs

// Core affinity - separate display from BLE to avoid starvation
#define CORE_BLE      1
#define CORE_DISPLAY  0

// History download configuration
// Use SENSOR_HISTORY_SIZE so we download exactly what the chart can display
// NOTE: Disabled due to heap exhaustion - BLE stack needs more runtime memory
// The detail screen on-demand download still works via detail_history module
#define DOWNLOAD_HISTORY_ON_STARTUP  true   // Download history on startup

// FreeRTOS timer handles
static TimerHandle_t s_sensor_timer = NULL;
static TimerHandle_t s_refresh_timer = NULL;
static TimerHandle_t s_progress_timer = NULL;

// Flag to trigger display refresh
static volatile bool s_do_refresh = false;

// History storage (static allocation) - use SENSOR_HISTORY_SIZE to match chart capacity
static inkbird_history_record_t s_history_records[SENSOR_HISTORY_SIZE];

// History download task handle (unused - kept for potential future background download feature)
static TaskHandle_t s_history_task __attribute__((unused)) = NULL;

static void progress_update_cb(TimerHandle_t timer)
{
    (void)timer;
    s_do_refresh = true;
}

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

    // Lazily create progress timer (singleton - reused across history downloads)
    if (s_progress_timer == NULL) {
        s_progress_timer = xTimerCreate(
            "progress",
            pdMS_TO_TICKS(PROGRESS_UPDATE_MS),
            pdTRUE,
            NULL,
            progress_update_cb
        );
    }
    xTimerStart(s_progress_timer, 0);

    LOG_HEAP("before BLE download");  // Check heap right before BLE connection

    uint16_t count = 0;
    esp_err_t ret = inkbird_ble_download_history(
        sensor_idx,
        s_history_records,
        SENSOR_HISTORY_SIZE,
        &count
    );

    xTimerStop(s_progress_timer, 0);

    // Clear downloading flag
    if (sensor) {
        sensor->downloading = false;
    }

    s_do_refresh = true;

    if (ret == ESP_OK && count > 0) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, ">>> History download SUCCESS: %u records <<<", count);

        sensor_data_clear_history(sensor_idx);

        ESP_LOGI(TAG, "Adding %u records to chart with timestamp reconstruction", count);

        ESP_LOGI(TAG, "  First: CO2=%u ppm, T=%.1f C, interval=%u",
                 s_history_records[0].co2_ppm,
                 s_history_records[0].temperature / 10.0f,
                 s_history_records[0].interval_mins);
        if (count > 1) {
            ESP_LOGI(TAG, "  Last:  CO2=%u ppm, T=%.1f C, interval=%u",
                     s_history_records[count - 1].co2_ppm,
                     s_history_records[count - 1].temperature / 10.0f,
                     s_history_records[count - 1].interval_mins);
        }

        for (int i = 0; i < count; i++) {
            inkbird_history_record_t *hist = &s_history_records[i];
            if (hist->co2_ppm > 0 && hist->co2_ppm < 10000) {
                sensor_data_add_history_with_interval(sensor_idx, hist->co2_ppm,
                                                       hist->temperature, hist->humidity,
                                                       hist->pressure, hist->interval_mins);
            }
        }

        uint16_t total_mins = sensor_data_get_total_minutes(sensor_idx);
        ESP_LOGI(TAG, "  Total time span: %u minutes", total_mins);

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
 * @brief Background task to download history from all sensors
 *
 * After history download completes, starts periodic BLE reading.
 */
static void history_download_task(void *arg)
{
    (void)arg;

    LOG_HEAP("history_task start");  // Check heap after 8KB task stack allocation

    uint8_t active_count = inkbird_ble_get_active_count();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Background: Syncing Historical Data");
    ESP_LOGI(TAG, "========================================");

    for (int i = 0; i < active_count; i++) {
        sensor_data_t *sensor = sensor_data_get(i);
        if (sensor) {
            sensor->downloading = true;
        }
        s_do_refresh = true;

        download_sensor_history(i);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  History sync complete!");
    ESP_LOGI(TAG, "========================================");
    s_do_refresh = true;

    ESP_LOGI(TAG, "Starting periodic BLE reading...");
    inkbird_ble_start();

    s_history_task = NULL;
    vTaskDelete(NULL);
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
    uint8_t active = inkbird_ble_get_active_count();
    for (int i = 0; i < active; i++) {
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

    s_do_refresh = true;
    led_update_from_co2();
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
            led_update_from_co2();
            return true;
        }

        ESP_LOGW(TAG, "Sensor %d read failed (attempt %d): %s",
                 sensor_idx, attempt, esp_err_to_name(ret));

        if (attempt < INKBIRD_STARTUP_READ_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_RETRY_DELAY_MS));
        }
    }

    ESP_LOGE(TAG, "Sensor %d: All %d read attempts failed",
             sensor_idx, INKBIRD_STARTUP_READ_RETRIES);
    return false;
}

/**
 * @brief Check if any sensor is currently downloading
 */
static bool any_sensor_downloading(void)
{
    // Check if detail history download is in progress
    if (detail_history_get_state() == DETAIL_HISTORY_IN_PROGRESS) {
        return true;
    }

    // Check startup history sync
    uint8_t active = inkbird_ble_get_active_count();
    for (int i = 0; i < active; i++) {
        sensor_data_t *sensor = sensor_data_get(i);
        if (sensor && sensor->downloading) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Display task - handles rendering and display updates
 */
static void display_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Display task started");

    vTaskDelay(pdMS_TO_TICKS(100));

    int64_t last_update_us = 0;
    int64_t last_touch_status_us = 0;

    while (1) {
        int64_t now_check = esp_timer_get_time();
        if (now_check - last_touch_status_us >= TOUCH_STATUS_LOG_INTERVAL_US) {
            ESP_LOGI(TAG, "Touch type: %d (0=none, 1=gt911, 2=xpt2046)",
                     ui_co2_display_get_touch_type());
            last_touch_status_us = now_check;
        }
        lv_timer_handler();

        int64_t now_us = esp_timer_get_time();
        bool downloading = any_sensor_downloading();
        int64_t update_interval = downloading ? UPDATE_INTERVAL_FAST_US : UPDATE_INTERVAL_NORMAL_US;

        if (s_do_refresh || (now_us - last_update_us) >= update_interval) {
            s_do_refresh = false;
            last_update_us = now_us;
            ui_co2_display_update();
            ui_co2_display_force_refresh();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
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
    ESP_LOGI(TAG, "  ESP32 + TFT + Inkbird BLE");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ========== STAGE 1: Initialize display and render loading screen ==========
    ESP_LOGI(TAG, "[Stage 1] Initializing display subsystem...");

    ESP_LOGI(TAG, "Initializing LED...");
    led_init();

    ESP_LOGI(TAG, "Initializing TFT display...");
    ret = epd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TFT init failed!");
        return;
    }

    ESP_LOGI(TAG, "Initializing sensor data...");
    sensor_data_init();
    detail_history_init();

    ESP_LOGI(TAG, "Initializing UI...");
    ui_co2_display_init();

    ESP_LOGI(TAG, "Showing loading screen...");
    ui_co2_display_loading();
    ui_co2_display_set_status("Starting...");

    for (int i = 0; i < 5; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "[Stage 1] Display ready - loading screen visible");

    // ========== STAGE 2: Show empty main screen ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[Stage 2] Showing main screen...");

    // Set initial status for all sensors before showing main screen
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_data_set_activity_status(i, "Waiting...");
    }

    // Trigger main screen creation by calling update
    ui_co2_display_update();

    // Render a few frames to show the main screen
    for (int i = 0; i < 10; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "[Stage 2] Main screen visible with sensors in Waiting state");

    // ========== STAGE 3: Initialize BLE stack ==========
    // NOTE: Do NOT start display task yet - BLE init needs uninterrupted CPU time
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[Stage 3] Initializing Bluetooth...");

    for (int i = 0; i < 3; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "Free heap before BLE: %lu bytes", esp_get_free_heap_size());
    ret = inkbird_ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed!");
        ui_co2_display_set_status("BLE Failed!");
        return;
    }
    ESP_LOGI(TAG, "[Stage 3] BLE initialized");
    ESP_LOGI(TAG, "Free heap after BLE: %lu bytes", esp_get_free_heap_size());

    // Now it's safe to start display task after BLE controller is running
    ESP_LOGI(TAG, "Starting display task on core %d...", CORE_DISPLAY);
    xTaskCreatePinnedToCore(
        display_task,
        "display",
        DISPLAY_TASK_STACK_SIZE,
        NULL,
        DISPLAY_TASK_PRIORITY,
        NULL,
        CORE_DISPLAY
    );
    vTaskDelay(pdMS_TO_TICKS(100));

    // ========== STAGE 4: Load known sensors ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[Stage 4] Loading known sensors...");

    // Load known sensors from config + NVS
    inkbird_ble_load_known_sensors();
    uint8_t known_count = inkbird_ble_get_active_count();
    ESP_LOGI(TAG, "[Stage 4] Loaded %d known sensor(s)", known_count);

    // If no known sensors, run discovery immediately
    if (known_count == 0) {
        ESP_LOGI(TAG, "[Stage 4] No known sensors, running discovery first");
        ui_co2_display_set_status("Scanning...");
        s_do_refresh = true;

        inkbird_ble_discover();
        inkbird_ble_register_discovered();
        inkbird_ble_save_to_nvs();
        known_count = inkbird_ble_get_active_count();
    }

    uint8_t active_count = known_count;
    ESP_LOGI(TAG, "[Stage 4] Active sensors: %d", active_count);
    LOG_HEAP("after loading sensors");

    for (int i = 0; i < active_count; i++) {
        sensor_data_set_name(i, inkbird_ble_get_sensor_name(i));
        sensor_data_set_activity_status(i, "Pending...");
    }

    // BLE init complete - show main screen
    ui_co2_display_loading_complete();
    s_do_refresh = true;
    vTaskDelay(pdMS_TO_TICKS(200));

    // ========== SENSOR INITIALIZATION: Each sensor fully initialized before moving to next ==========
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Initializing Known Sensors");
    ESP_LOGI(TAG, "========================================");

    for (int i = 0; i < active_count; i++) {
        const char *name = inkbird_ble_get_sensor_name(i);
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "--- Sensor %d: %s ---", i, name);

        sensor_data_t *sensor = sensor_data_get(i);

        // Step 1: Get current reading (also receives settings automatically)
        sensor_data_set_activity_status(i, "Reading...");
        s_do_refresh = true;
        read_initial_sensor_value(i);
        vTaskDelay(pdMS_TO_TICKS(200));

#if DOWNLOAD_HISTORY_ON_STARTUP
        // Step 2: Download history
        if (sensor) {
            sensor->downloading = true;
        }
        s_do_refresh = true;
        download_sensor_history(i);
        vTaskDelay(pdMS_TO_TICKS(200));
#endif

        // Clear status - sensor is ready
        sensor_data_set_activity_status(i, NULL);
        s_do_refresh = true;

        ESP_LOGI(TAG, "  Sensor %d ready", i);
        LOG_HEAP("after sensor init");
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Known sensors initialized!");
    ESP_LOGI(TAG, "========================================");

    // ========== STAGE 5: Discover additional sensors if room available ==========
    if (active_count < INKBIRD_SENSOR_COUNT) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "[Stage 5] Room for %d more sensor(s), scanning for new devices...",
                 INKBIRD_SENSOR_COUNT - active_count);
        ui_co2_display_set_status("Scanning...");
        s_do_refresh = true;

        uint8_t before_count = active_count;
        inkbird_ble_discover();
        inkbird_ble_register_discovered();

        uint8_t new_count = inkbird_ble_get_active_count();
        uint8_t discovered_new = new_count - before_count;

        if (discovered_new > 0) {
            ESP_LOGI(TAG, "[Stage 5] Found %d new sensor(s), saving to NVS", discovered_new);
            inkbird_ble_save_to_nvs();

            // Initialize newly discovered sensors
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "  Initializing New Sensors");
            ESP_LOGI(TAG, "========================================");

            for (int i = before_count; i < new_count; i++) {
                const char *name = inkbird_ble_get_sensor_name(i);
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "--- New Sensor %d: %s ---", i, name);

                sensor_data_set_name(i, name);
                sensor_data_t *sensor = sensor_data_get(i);

                sensor_data_set_activity_status(i, "Reading...");
                s_do_refresh = true;
                read_initial_sensor_value(i);
                vTaskDelay(pdMS_TO_TICKS(200));

#if DOWNLOAD_HISTORY_ON_STARTUP
                if (sensor) {
                    sensor->downloading = true;
                }
                s_do_refresh = true;
                download_sensor_history(i);
                vTaskDelay(pdMS_TO_TICKS(200));
#endif

                sensor_data_set_activity_status(i, NULL);
                s_do_refresh = true;

                ESP_LOGI(TAG, "  Sensor %d ready", i);
                LOG_HEAP("after new sensor init");
            }

            active_count = new_count;
        } else {
            ESP_LOGI(TAG, "[Stage 5] No new sensors found");
        }
    } else {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "[Stage 5] All %d sensor slots filled, skipping discovery", INKBIRD_SENSOR_COUNT);
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  All %d sensors ready!", active_count);
    ESP_LOGI(TAG, "========================================");

    // Start periodic BLE reading BEFORE timers to avoid race conditions
    // (timers may fire while BLE task is still initializing)
    ESP_LOGI(TAG, "Starting periodic BLE reading...");
    inkbird_ble_start();

    // ========== Set up display timers ==========
    // Now safe to start timers - BLE task is running
    s_sensor_timer = xTimerCreate(
        "sensor_update",
        pdMS_TO_TICKS(SENSOR_UPDATE_MS),
        pdTRUE,
        NULL,
        sensor_update_cb
    );
    xTimerStart(s_sensor_timer, 0);

    s_refresh_timer = xTimerCreate(
        "display_refresh",
        pdMS_TO_TICKS(DISPLAY_REFRESH_MS),
        pdTRUE,
        NULL,
        display_refresh_cb
    );
    xTimerStart(s_refresh_timer, 0);

    ESP_LOGI(TAG, "Initialization complete!");
    ESP_LOGI(TAG, "Tap a sensor tile to view its detail page with history chart.");
    ESP_LOGI(TAG, "(History will download with downsampling to cover ~24 hours)");

    // Main task can sleep
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/**
 * @file detail_history.c
 * @brief Extended history buffer for detail screen
 *
 * Implements the large shared buffer for displaying extended history
 * on the sensor detail screen. Downloads data via BLE in a background task.
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "detail_history.h"
#include "inkbird_ble.h"

static const char *TAG = "detail_history";

// Static data buffers for the large history
static int16_t s_detail_co2[DETAIL_HISTORY_SIZE];
static int16_t s_detail_temp[DETAIL_HISTORY_SIZE];
static int16_t s_detail_hum[DETAIL_HISTORY_SIZE];
static int16_t s_detail_pres[DETAIL_HISTORY_SIZE];
static uint16_t s_detail_time_offsets[DETAIL_HISTORY_SIZE];

// Download state
static detail_history_state_t s_state = DETAIL_HISTORY_IDLE;
static uint8_t s_sensor_idx = 0xFF;
static uint16_t s_history_count = 0;
static uint16_t s_total_minutes = 0;
static volatile bool s_cancel_requested = false;

// Download task
static TaskHandle_t s_download_task_handle = NULL;

// Static buffer for BLE download (10 bytes per record)
// Statically allocated to avoid heap fragmentation during download
#define DOWNLOAD_BUFFER_SIZE DETAIL_HISTORY_SIZE
static inkbird_history_record_t s_download_buffer[DOWNLOAD_BUFFER_SIZE];

// Timestamp reconstruction state
static uint8_t s_last_interval = 0;
static uint16_t s_cumulative_minutes = 0;

void detail_history_init(void)
{
    detail_history_clear();
    ESP_LOGI(TAG, "Detail history module initialized (buffer: %d points, ~%d KB)",
             DETAIL_HISTORY_SIZE, (DETAIL_HISTORY_SIZE * 5 * 2) / 1024);
}

/**
 * @brief Download task that runs on BLE core
 */
static void download_task(void *arg)
{
    uint8_t sensor_idx = (uint8_t)(uintptr_t)arg;

    ESP_LOGI(TAG, "Download task started for sensor %d", sensor_idx);

    // Stop periodic BLE reading to avoid conflicts during history download
    // This must happen in the download task (not caller) to avoid blocking UI
    ESP_LOGI(TAG, "Stopping periodic BLE reading for history download...");
    inkbird_ble_stop();
    vTaskDelay(pdMS_TO_TICKS(3000));  // Allow time for BLE stack to fully settle

    // Check if cancelled during BLE stop wait
    if (s_cancel_requested) {
        ESP_LOGI(TAG, "Download cancelled during BLE stop");
        s_state = DETAIL_HISTORY_CANCELLED;
        inkbird_ble_start();
        s_download_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Log heap info (buffer is statically allocated, no malloc needed)
    size_t free_heap = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Heap after BLE stop: free=%u, largest_block=%u (static buffer: %u bytes for %d records)",
             (unsigned)free_heap, (unsigned)largest_block,
             (unsigned)(sizeof(inkbird_history_record_t) * DOWNLOAD_BUFFER_SIZE), DOWNLOAD_BUFFER_SIZE);

    // Start BLE download
    ESP_LOGI(TAG, "Starting BLE download for sensor %d, max_records=%d", sensor_idx, DOWNLOAD_BUFFER_SIZE);
    uint16_t out_count = 0;
    esp_err_t ret = inkbird_ble_download_history(sensor_idx, s_download_buffer,
                                                  DOWNLOAD_BUFFER_SIZE, &out_count);
    ESP_LOGI(TAG, "BLE download returned: ret=%s, out_count=%u", esp_err_to_name(ret), out_count);

    // Check if cancelled during download
    if (s_cancel_requested) {
        ESP_LOGI(TAG, "Download was cancelled");
        s_state = DETAIL_HISTORY_CANCELLED;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(ret));
        s_state = DETAIL_HISTORY_ERROR;
    } else {
        ESP_LOGI(TAG, "Download complete, processing %u records", out_count);

        // Get downsample rate to correctly reconstruct timestamps
        uint8_t downsample_rate = inkbird_ble_get_history_downsample_rate();
        ESP_LOGI(TAG, "Downsample rate: %d (each stored record spans ~%d minutes)",
                 downsample_rate, downsample_rate);

        // Process downloaded records into detail buffers
        // Records are already in chronological order (oldest to newest)
        s_history_count = 0;
        s_last_interval = 0;
        s_cumulative_minutes = 0;

        for (uint16_t i = 0; i < out_count && i < DETAIL_HISTORY_SIZE; i++) {
            inkbird_history_record_t *rec = &s_download_buffer[i];

            // Store values
            s_detail_co2[i] = (int16_t)rec->co2_ppm;
            s_detail_temp[i] = rec->temperature;
            s_detail_hum[i] = (int16_t)rec->humidity;
            s_detail_pres[i] = (int16_t)rec->pressure;

            // Reconstruct timestamps using interval field
            // With downsampling, each stored record represents ~downsample_rate minutes
            if (i == 0) {
                s_cumulative_minutes = 0;
                s_last_interval = rec->interval_mins;
            } else {
                uint8_t prev = s_last_interval;
                uint8_t curr = rec->interval_mins;
                uint16_t elapsed;

                if (curr > prev) {
                    // Normal forward progression
                    elapsed = curr - prev;
                } else if (curr < prev) {
                    // Hour rollover
                    elapsed = (60 - prev) + curr;
                } else {
                    // Same minute - but with downsampling, we skipped records
                    // Estimate based on downsample rate
                    elapsed = downsample_rate;
                }

                // Scale elapsed time by downsample rate to account for skipped records
                // When downsampling by N, each transition represents ~N minutes
                s_cumulative_minutes += elapsed * downsample_rate;
                s_last_interval = curr;
            }

            s_detail_time_offsets[i] = s_cumulative_minutes;
            s_history_count++;
        }

        s_total_minutes = s_cumulative_minutes;

        // Convert time offsets to "minutes ago" format
        // Index 0 is oldest (most minutes ago), index N-1 is newest (0 = now)
        for (uint16_t i = 0; i < s_history_count; i++) {
            s_detail_time_offsets[i] = s_total_minutes - s_detail_time_offsets[i];
        }

        ESP_LOGI(TAG, "Processed %u records, total span: %u minutes (~%.1f hours)",
                 s_history_count, s_total_minutes, s_total_minutes / 60.0f);

        s_state = DETAIL_HISTORY_COMPLETE;
    }

    // Restart periodic BLE reading
    ESP_LOGI(TAG, "Restarting periodic BLE reading...");
    inkbird_ble_start();

    // Buffer is static, no free needed
    s_download_task_handle = NULL;
    vTaskDelete(NULL);
}

bool detail_history_start_download(uint8_t sensor_idx)
{
    if (sensor_idx >= 4) {
        ESP_LOGE(TAG, "Invalid sensor index: %d", sensor_idx);
        return false;
    }

    if (s_state == DETAIL_HISTORY_IN_PROGRESS) {
        ESP_LOGW(TAG, "Download already in progress");
        return false;
    }

    // Clear previous data
    detail_history_clear();

    s_sensor_idx = sensor_idx;
    s_state = DETAIL_HISTORY_IN_PROGRESS;
    s_cancel_requested = false;

    // Create download task on BLE core (core 1)
    // Stack size reduced to 2560 to conserve heap for BLE operations
    // Note: Minimum viable stack for this task - do not reduce further
    BaseType_t result = xTaskCreatePinnedToCore(
        download_task,
        "detail_hist_dl",
        2560,
        (void *)(uintptr_t)sensor_idx,
        5,
        &s_download_task_handle,
        1  // BLE core
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create download task");
        s_state = DETAIL_HISTORY_ERROR;
        return false;
    }

    ESP_LOGI(TAG, "Started download for sensor %d", sensor_idx);
    return true;
}

void detail_history_cancel_download(void)
{
    if (s_state != DETAIL_HISTORY_IN_PROGRESS) {
        return;
    }

    ESP_LOGI(TAG, "Requesting download cancellation");
    s_cancel_requested = true;

    // Also cancel the BLE-level download
    inkbird_ble_cancel_history();
}

detail_history_state_t detail_history_get_state(void)
{
    return s_state;
}

uint8_t detail_history_get_progress(void)
{
    if (s_state != DETAIL_HISTORY_IN_PROGRESS) {
        return s_state == DETAIL_HISTORY_COMPLETE ? 100 : 0;
    }

    uint16_t expected = 0, received = 0;
    inkbird_ble_get_history_progress(&expected, &received);

    if (expected == 0) {
        return 0;
    }

    // Cap at 99% while still downloading
    uint32_t progress = (received * 100) / expected;
    if (progress > 99) {
        progress = 99;
    }
    return (uint8_t)progress;
}

uint16_t detail_history_get_count(void)
{
    return s_history_count;
}

uint8_t detail_history_get_sensor_idx(void)
{
    return s_sensor_idx;
}

const int16_t *detail_history_get_co2(uint16_t *out_count)
{
    if (out_count != NULL) {
        *out_count = s_history_count;
    }
    return s_detail_co2;
}

const int16_t *detail_history_get_temp(uint16_t *out_count)
{
    if (out_count != NULL) {
        *out_count = s_history_count;
    }
    return s_detail_temp;
}

const int16_t *detail_history_get_hum(uint16_t *out_count)
{
    if (out_count != NULL) {
        *out_count = s_history_count;
    }
    return s_detail_hum;
}

const int16_t *detail_history_get_pres(uint16_t *out_count)
{
    if (out_count != NULL) {
        *out_count = s_history_count;
    }
    return s_detail_pres;
}

const uint16_t *detail_history_get_time_offsets(uint16_t *out_count)
{
    if (out_count != NULL) {
        *out_count = s_history_count;
    }
    return s_detail_time_offsets;
}

uint16_t detail_history_get_total_minutes(void)
{
    return s_total_minutes;
}

void detail_history_clear(void)
{
    // Cancel any ongoing download
    if (s_state == DETAIL_HISTORY_IN_PROGRESS) {
        detail_history_cancel_download();
        // Wait briefly for task to finish
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_state = DETAIL_HISTORY_IDLE;
    s_sensor_idx = 0xFF;
    s_history_count = 0;
    s_total_minutes = 0;
    s_cancel_requested = false;
    s_last_interval = 0;
    s_cumulative_minutes = 0;

    // Clear buffers
    for (int i = 0; i < DETAIL_HISTORY_SIZE; i++) {
        s_detail_co2[i] = -1;
        s_detail_temp[i] = -1;
        s_detail_hum[i] = -1;
        s_detail_pres[i] = -1;
        s_detail_time_offsets[i] = 0;
    }

    ESP_LOGD(TAG, "Detail history cleared");
}

/**
 * @file inkbird_ble_history.c
 * @brief Historical data download implementation for Inkbird sensors
 *
 * Handles downloading and parsing of historical sensor data:
 * - Record count parsing and validation
 * - 10-byte history record parsing (CO2, temp, humidity, pressure, interval)
 * - Circular buffer management for keeping newest records
 * - End marker detection and download completion
 */

#include "inkbird_ble_internal.h"

static const char *TAG = "inkbird_history";

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Send a command to the sensor via FFE9
 */
esp_err_t inkbird_send_history_command(const uint8_t *cmd, size_t len)
{
    if (!s_connected || s_gattc_if == ESP_GATT_IF_NONE || s_cmd_char_handle == 0) {
        ESP_LOGE(TAG, "Cannot send command: not connected or no command handle");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending command to FFE9 (handle=%d):", s_cmd_char_handle);
    ESP_LOG_BUFFER_HEX(TAG, cmd, len);

    esp_err_t ret = esp_ble_gattc_write_char(
        s_gattc_if, s_conn_id, s_cmd_char_handle,
        len, (uint8_t *)cmd,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write command failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Check if a 10-byte record is empty (all 0xFF = uninitialized flash)
 */
bool inkbird_is_empty_record(const uint8_t *data)
{
    for (int i = 0; i < 10; i++) {
        if (data[i] != HISTORY_EMPTY_BYTE) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Parse a single 10-byte history record using circular buffer
 *
 * Format from APK (IadW1Model.setHistory):
 * - Bytes 0-1: CO2 (big-endian)
 * - Byte 2 lower nibble: Unit flag (0=Celsius, 1=Fahrenheit)
 * - Byte 2 upper nibble: Temperature sign (0=positive, 1=negative)
 * - Bytes 3-4: Temperature * 10 (big-endian)
 * - Bytes 5-6: Humidity * 10 (big-endian)
 * - Bytes 7-8: Pressure/HAP (big-endian)
 * - Byte 9: Time interval in minutes
 *
 * Uses circular buffer to keep the NEWEST records.
 *
 * @return true if record was stored, false if skipped (empty/invalid)
 */
bool inkbird_parse_history_record(const uint8_t *data)
{
    // Skip empty records
    if (inkbird_is_empty_record(data)) {
        return false;
    }

    if (s_history_records == NULL || s_history_max_records == 0) {
        return false;
    }

    // Parse CO2 first to validate
    uint16_t co2_ppm = ((uint16_t)data[0] << 8) | data[1];

    // Skip records with invalid CO2 values (200-10000 ppm is valid range)
    if (co2_ppm < 200 || co2_ppm > 10000) {
        return false;
    }

    // Write to current position in circular buffer
    inkbird_history_record_t *rec = &s_history_records[s_history_write_idx];

    // CO2: bytes 0-1 (big-endian)
    rec->co2_ppm = co2_ppm;

    // Byte 2 structure:
    // Upper nibble = TempUnit, Lower nibble = TempSign
    rec->is_fahrenheit = (data[2] & 0xF0) != 0;
    bool is_negative = (data[2] & 0x0F) != 0;

    // Temperature: bytes 3-4 (big-endian) * 0.1
    uint16_t temp_raw = ((uint16_t)data[3] << 8) | data[4];
    rec->temperature = is_negative ? -(int16_t)temp_raw : (int16_t)temp_raw;

    // Humidity: bytes 5-6 (big-endian) * 0.1
    rec->humidity = ((uint16_t)data[5] << 8) | data[6];

    // Pressure: bytes 7-8 (big-endian)
    rec->pressure = ((uint16_t)data[7] << 8) | data[8];

    // Interval: byte 9
    rec->interval_mins = data[9];

    s_history_received_count++;

    ESP_LOGD(TAG, "History[%d->%d]: CO2=%u, T=%d, H=%u, P=%u, Int=%u",
             s_history_received_count, s_history_write_idx, rec->co2_ppm,
             rec->temperature, rec->humidity, rec->pressure, rec->interval_mins);

    // Advance write index (circular)
    s_history_write_idx++;
    if (s_history_write_idx >= s_history_max_records) {
        s_history_write_idx = 0;
        s_history_buffer_wrapped = true;
    }

    return true;
}

// ============================================================================
// History Notification Parsing
// ============================================================================

/**
 * @brief Parse history notification data
 *
 * Protocol (per INKBIRD_IAM_T1_PROTOCOL.md section 6):
 * 1. First packet: 2 bytes = record count (big-endian)
 * 2. Data packets: 10 bytes per record (raw bytes, fragmented across BLE packets)
 * 3. End marker: 0x66 0x66
 */
void inkbird_parse_history_notification(const uint8_t *data, size_t len)
{
    ESP_LOGD(TAG, "History RX: %d bytes, first 4: %02X %02X %02X %02X",
             (int)len, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);

    // Check for end marker (0x6666) anywhere in packet
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == HISTORY_END_MARKER_HIGH && data[i + 1] == HISTORY_END_MARKER_LOW) {
            ESP_LOGI(TAG, "History end marker in packet at offset %d", (int)i);
            // Add data before the end marker to buffer
            if (i > 0 && s_history_buffer_len + i < HISTORY_BUFFER_SIZE) {
                memcpy(s_history_buffer + s_history_buffer_len, data, i);
                s_history_buffer_len += i;
            }
            s_history_state = INKBIRD_HISTORY_COMPLETE;
            if (s_history_complete_sem != NULL) {
                xSemaphoreGive(s_history_complete_sem);
            }
            return;
        }
    }

    // First response should be record count (2 bytes, big-endian)
    if (s_history_state == INKBIRD_HISTORY_REQUESTING && !s_history_got_count) {
        if (len >= 2) {
            s_history_expected_count = ((uint16_t)data[0] << 8) | data[1];
            s_history_got_count = true;
            s_history_state = INKBIRD_HISTORY_RECEIVING;
            ESP_LOGI(TAG, ">>> History record count: %u (raw: 0x%02X%02X) <<<",
                     s_history_expected_count, data[0], data[1]);

            // Process remaining data in this packet
            if (len > 2) {
                size_t remaining = len - 2;
                if (remaining + s_history_buffer_len < HISTORY_BUFFER_SIZE) {
                    memcpy(s_history_buffer + s_history_buffer_len, data + 2, remaining);
                    s_history_buffer_len += remaining;
                }
            }
        }
        return;
    }

    // Receiving state: accumulate data and parse records
    if (s_history_state == INKBIRD_HISTORY_RECEIVING) {
        // Add new data to buffer
        if (len + s_history_buffer_len < HISTORY_BUFFER_SIZE) {
            memcpy(s_history_buffer + s_history_buffer_len, data, len);
            s_history_buffer_len += len;
        } else {
            ESP_LOGW(TAG, "History buffer overflow, discarding data");
        }

        // Process complete 10-byte records from buffer
        while (s_history_buffer_len >= 10) {
            // Check for end marker in buffer
            if (s_history_buffer[0] == HISTORY_END_MARKER_HIGH &&
                s_history_buffer[1] == HISTORY_END_MARKER_LOW) {
                ESP_LOGI(TAG, "History end marker found at buffer start");
                s_history_state = INKBIRD_HISTORY_COMPLETE;
                if (s_history_complete_sem != NULL) {
                    xSemaphoreGive(s_history_complete_sem);
                }
                return;
            }

            // Parse one record
            inkbird_parse_history_record(s_history_buffer);

            // Shift buffer
            memmove(s_history_buffer, s_history_buffer + 10, s_history_buffer_len - 10);
            s_history_buffer_len -= 10;

            // Log progress periodically
            if (s_history_received_count > 0 && s_history_received_count % 5000 == 0) {
                ESP_LOGI(TAG, "Progress: %u valid records received...", s_history_received_count);
            }
        }

        // Check for end marker in remaining buffer
        if (s_history_buffer_len >= 2) {
            for (size_t i = 0; i <= s_history_buffer_len - 2; i++) {
                if (s_history_buffer[i] == HISTORY_END_MARKER_HIGH &&
                    s_history_buffer[i + 1] == HISTORY_END_MARKER_LOW) {
                    ESP_LOGI(TAG, "History end marker found at offset %d in buffer", (int)i);
                    s_history_state = INKBIRD_HISTORY_COMPLETE;
                    if (s_history_complete_sem != NULL) {
                        xSemaphoreGive(s_history_complete_sem);
                    }
                    return;
                }
            }
        }
    }
}

// ============================================================================
// History Public API
// ============================================================================

esp_err_t inkbird_ble_download_history(uint8_t sensor_idx,
                                        inkbird_history_record_t *records,
                                        uint16_t max_records,
                                        uint16_t *out_count)
{
    if (!s_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (records == NULL || max_records == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sensor_idx >= s_active_sensor_count || !s_active_sensors[sensor_idx].enabled) {
        ESP_LOGE(TAG, "Invalid or disabled sensor index: %d (active: %d)", sensor_idx, s_active_sensor_count);
        return ESP_ERR_INVALID_ARG;
    }

    // Create completion semaphore if needed
    if (s_history_complete_sem == NULL) {
        s_history_complete_sem = xSemaphoreCreateBinary();
        if (s_history_complete_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create history semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    // Reset history state
    s_history_state = INKBIRD_HISTORY_REQUESTING;
    s_history_records = records;
    s_history_max_records = max_records;
    s_history_expected_count = 0;
    s_history_received_count = 0;
    s_history_stored_count = 0;
    s_history_got_count = false;
    s_history_buffer_len = 0;
    s_history_write_idx = 0;
    s_history_buffer_wrapped = false;
    *out_count = 0;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting History Download");
    ESP_LOGI(TAG, "  Sensor: %d (%s)", sensor_idx, s_active_sensors[sensor_idx].name);
    ESP_LOGI(TAG, "  Max records: %u", max_records);
    ESP_LOGI(TAG, "========================================");

    // Connect to sensor
    s_current_sensor_index = sensor_idx;
    memcpy(s_target_bda, s_active_sensors[sensor_idx].mac, 6);
    s_data_received = false;

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X...",
             s_target_bda[0], s_target_bda[1], s_target_bda[2],
             s_target_bda[3], s_target_bda[4], s_target_bda[5]);

    // Drain stale semaphore signals
    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

    esp_err_t ret = esp_ble_gattc_open(
        s_gattc_if, s_target_bda,
        BLE_ADDR_TYPE_PUBLIC, true);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Public addr failed, trying random...");
        ret = esp_ble_gattc_open(
            s_gattc_if, s_target_bda,
            BLE_ADDR_TYPE_RANDOM, true);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(ret));
        s_history_state = INKBIRD_HISTORY_IDLE;  // Reset to allow normal mode reads
        return ret;
    }

    // Wait for connection and notification setup
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem, pdMS_TO_TICKS(30000));
    if (got_sem != pdTRUE || !s_connected) {
        ESP_LOGE(TAG, "Connection timeout or failed");
        if (s_connected) {
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
        } else {
            esp_ble_gap_disconnect(s_target_bda);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        s_history_state = INKBIRD_HISTORY_IDLE;  // Reset to allow normal mode reads
        return ESP_ERR_TIMEOUT;
    }

    // Small delay after notification setup
    vTaskDelay(pdMS_TO_TICKS(500));

    // Send history start command
    ESP_LOGI(TAG, "Sending history start command...");

    ret = inkbird_send_history_command(CMD_HISTORY_START, sizeof(CMD_HISTORY_START));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send history command");
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        s_history_state = INKBIRD_HISTORY_IDLE;  // Reset to allow normal mode reads
        return ret;
    }

    // Wait for history download to complete (timeout: 5 minutes)
    ESP_LOGI(TAG, "Waiting for history data (timeout: 300s)...");
    got_sem = xSemaphoreTake(s_history_complete_sem, pdMS_TO_TICKS(300000));

    if (got_sem != pdTRUE) {
        ESP_LOGW(TAG, "History download timeout");
        s_history_state = INKBIRD_HISTORY_ERROR;
    }

    // Disconnect
    ESP_LOGI(TAG, "Disconnecting...");
    if (s_connected) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Calculate stored count and reorder circular buffer
    if (s_history_buffer_wrapped) {
        s_history_stored_count = s_history_max_records;

        ESP_LOGI(TAG, "Reordering circular buffer (wrapped at idx %u)...", s_history_write_idx);

        // Allocate temp buffer for reordering
        inkbird_history_record_t *temp = pvPortMalloc(sizeof(inkbird_history_record_t) * s_history_max_records);
        if (temp != NULL) {
            // Copy from write_idx to end (older records)
            uint16_t first_part = s_history_max_records - s_history_write_idx;
            memcpy(temp, &records[s_history_write_idx], sizeof(inkbird_history_record_t) * first_part);

            // Copy from start to write_idx (newer records)
            memcpy(&temp[first_part], records, sizeof(inkbird_history_record_t) * s_history_write_idx);

            // Copy back to original buffer
            memcpy(records, temp, sizeof(inkbird_history_record_t) * s_history_max_records);

            vPortFree(temp);
            ESP_LOGI(TAG, "Buffer reordered: oldest at [0], newest at [%u]", s_history_max_records - 1);
        } else {
            ESP_LOGW(TAG, "Failed to allocate temp buffer for reordering");
        }
    } else {
        s_history_stored_count = s_history_write_idx;
        ESP_LOGI(TAG, "Buffer did not wrap, %u records in order", s_history_stored_count);
    }

    // Report results
    *out_count = s_history_stored_count;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  History Download Complete");
    ESP_LOGI(TAG, "  Total records from sensor: %u", s_history_expected_count);
    ESP_LOGI(TAG, "  Valid records received: %u", s_history_received_count);
    ESP_LOGI(TAG, "  Records in output buffer: %u (NEWEST)", s_history_stored_count);
    ESP_LOGI(TAG, "  State: %d", s_history_state);
    ESP_LOGI(TAG, "========================================");

    // Determine success
    bool success = (s_history_state == INKBIRD_HISTORY_COMPLETE ||
        (s_history_stored_count > 0 && s_history_received_count > s_history_expected_count * 9 / 10));

    // Reset state
    s_history_state = INKBIRD_HISTORY_IDLE;
    s_history_records = NULL;

    if (success) {
        ESP_LOGI(TAG, "History download considered successful");
        return ESP_OK;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t inkbird_ble_cancel_history(void)
{
    if (s_history_state == INKBIRD_HISTORY_IDLE) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Cancelling history download...");

    esp_err_t ret = inkbird_send_history_command(CMD_HISTORY_STOP, sizeof(CMD_HISTORY_STOP));

    s_history_state = INKBIRD_HISTORY_IDLE;
    s_history_records = NULL;
    s_history_buffer_len = 0;

    return ret;
}

inkbird_history_state_t inkbird_ble_get_history_state(void)
{
    return s_history_state;
}

void inkbird_ble_get_history_progress(uint16_t *out_expected, uint16_t *out_received)
{
    if (out_expected != NULL) {
        *out_expected = s_history_expected_count;
    }
    if (out_received != NULL) {
        *out_received = s_history_received_count;
    }
}

/**
 * @file inkbird_ble_data.c
 * @brief Data parsing and real-time reading for Inkbird sensors (NimBLE)
 *
 * Handles parsing of sensor data notifications including:
 * - Real-time environmental data (CO2, temperature, humidity, pressure)
 * - CO2 settings, thresholds, alarm, and calibration responses
 * - The periodic read task that cycles through sensors
 */

#include "inkbird_ble_internal.h"

static const char *TAG = "inkbird_data";

// ============================================================================
// Data Parsing (ESPHome format - starts with 0x55)
// ============================================================================

/**
 * @brief Parse Inkbird IAM-T1 sensor data (ESPHome format)
 *
 * ESPHome parses IAM-T1 data with 0x55 header:
 * - Byte 0: 0x55 (header check)
 * - Byte 4: Temperature sign (lower nibble: 1 = negative)
 * - Bytes 5-6: Temperature value (big-endian) * 0.1C
 * - Bytes 7-8: Humidity (big-endian) * 0.1%
 * - Bytes 9-10: CO2 in ppm (big-endian)
 * - Bytes 11-12: Pressure in hPa (big-endian)
 *
 * @return true if real-time data was parsed (cmd 0x01), false otherwise
 */
bool inkbird_parse_data(const uint8_t *data, size_t len, uint8_t sensor_idx)
{
    if (sensor_idx >= INKBIRD_SENSOR_COUNT) {
        return false;
    }

    ESP_LOGI(TAG, "=== PARSING DATA ===");
    ESP_LOGI(TAG, "Length: %d bytes", len);
    if (len > 0) {
        ESP_LOG_BUFFER_HEX(TAG, data, len > 20 ? 20 : len);
    }

    // Response format: 55 AA [cmd] [len] [data...]
    if (len >= 4 && data[0] == 0x55 && data[1] == 0xAA) {
        uint8_t cmd_id = data[2];

        // Parse pairing response (cmd 0x08) per protocol section 7.2
        if (cmd_id == 0x08 && len >= 5) {
            uint8_t status = data[4];
            ESP_LOGI(TAG, "Pairing response: %s (0x%02X) for sensor %d (history_mode=%d)",
                     status == 0x00 ? "ready" : status == 0x02 ? "success" : "unknown",
                     status, sensor_idx, s_history_setup_mode);

            // Find the peer for this sensor to get the correct handles
            inkbird_peer_t *peer = NULL;
            for (int i = 0; i < s_peer_count; i++) {
                if (s_peers[i].sensor_idx == sensor_idx && s_peers[i].connected) {
                    peer = &s_peers[i];
                    break;
                }
            }

            // Check if pairing succeeded
            if (status != 0x00 && status != 0x02) {
                ESP_LOGW(TAG, "Pairing failed with status 0x%02X", status);
                return false;
            }

            // In history setup mode, signal completion so history download can proceed
            if (s_history_setup_mode && peer != NULL) {
                ESP_LOGI(TAG, "History setup mode: pairing complete, signaling ready");
                // Update legacy globals for history compatibility
                s_current_sensor_index = peer->sensor_idx;
                s_conn_handle = peer->conn_handle;
                s_cmd_char_handle = peer->cmd_char_val_handle;
                xSemaphoreGive(s_read_complete_sem);
                return false;
            }

            // Normal mode: send real-time data request after pairing
            if (peer != NULL && peer->cmd_char_val_handle != 0 &&
                s_history_state == INKBIRD_HISTORY_IDLE && !s_settings_request_mode) {
                sensor_data_set_activity_status(sensor_idx, "Requesting...");

                // Send real-time data request (0x09) first - this is what we need
                ESP_LOGI(TAG, "Sending real-time data request...");
                ble_gattc_write_flat(peer->conn_handle,
                                      peer->cmd_char_val_handle,
                                      CMD_REALTIME_DATA, sizeof(CMD_REALTIME_DATA),
                                      NULL, NULL);
            }
            return false;
        }

        // Parse CO2 settings (cmd 0x02)
        if (cmd_id == 0x02 && len >= 10 && s_ble_mutex != NULL) {
            xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
            s_co2_settings[sensor_idx].display_mode = data[4];
            s_co2_settings[sensor_idx].use_custom = (data[5] != 0x00);
            s_co2_settings[sensor_idx].auto_calibration = (data[6] != 0x00);
            s_co2_settings[sensor_idx].manual_mode = data[7];
            s_co2_settings[sensor_idx].manual_cal_value = ((uint16_t)data[8] << 8) | data[9];
            s_co2_settings[sensor_idx].valid = true;
            s_thresholds[sensor_idx].use_custom = s_co2_settings[sensor_idx].use_custom;
            s_thresholds[sensor_idx].settings_valid = true;
            xSemaphoreGive(s_ble_mutex);
            ESP_LOGI(TAG, "Sensor %d CO2 settings: mode=%d, custom=%d, auto_cal=%d, manual=%d, cal_val=%u",
                     sensor_idx, s_co2_settings[sensor_idx].display_mode,
                     s_co2_settings[sensor_idx].use_custom,
                     s_co2_settings[sensor_idx].auto_calibration,
                     s_co2_settings[sensor_idx].manual_mode,
                     s_co2_settings[sensor_idx].manual_cal_value);
            if (s_settings_request_mode) {
                s_settings_responses_received++;
            }
            return false;
        }

        // Parse CO2 thresholds (cmd 0x03)
        if (cmd_id == 0x03 && len >= 12 && s_ble_mutex != NULL) {
            uint16_t norm_high = ((uint16_t)data[4] << 8) | data[5];
            uint16_t norm_low = ((uint16_t)data[6] << 8) | data[7];
            uint16_t plant_high = ((uint16_t)data[8] << 8) | data[9];
            uint16_t plant_low = ((uint16_t)data[10] << 8) | data[11];
            xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
            s_thresholds[sensor_idx].normal_high_ppm = norm_high;
            s_thresholds[sensor_idx].normal_low_ppm = norm_low;
            s_thresholds[sensor_idx].plant_high_ppm = plant_high;
            s_thresholds[sensor_idx].plant_low_ppm = plant_low;
            s_thresholds[sensor_idx].thresholds_valid = true;
            xSemaphoreGive(s_ble_mutex);
            ESP_LOGI(TAG, "Sensor %d thresholds synced: normal=%u-%u, plant=%u-%u ppm",
                     sensor_idx, norm_low, norm_high, plant_low, plant_high);
            if (s_settings_request_mode) {
                s_settings_responses_received++;
            }
            return false;
        }

        // Parse CO2 alarm settings (cmd 0x04)
        if (cmd_id == 0x04 && len >= 8 && s_ble_mutex != NULL) {
            xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
            s_alarm_settings[sensor_idx].enabled = (data[4] == 0x01);
            s_alarm_settings[sensor_idx].alarm_mode = data[5];
            s_alarm_settings[sensor_idx].alarm_value = ((uint16_t)data[6] << 8) | data[7];
            s_alarm_settings[sensor_idx].valid = true;
            xSemaphoreGive(s_ble_mutex);
            ESP_LOGI(TAG, "Sensor %d alarm: enabled=%d, mode=%d, value=%u ppm",
                     sensor_idx, s_alarm_settings[sensor_idx].enabled,
                     s_alarm_settings[sensor_idx].alarm_mode,
                     s_alarm_settings[sensor_idx].alarm_value);
            if (s_settings_request_mode) {
                s_settings_responses_received++;
            }
            return false;
        }

        // Parse calibration settings (cmd 0x05)
        if (cmd_id == 0x05 && len >= 11 && s_ble_mutex != NULL) {
            xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
            bool co2_neg = (data[4] != 0x00);
            uint8_t co2_val = data[5];
            s_calibration[sensor_idx].co2_offset = co2_neg ? -(int16_t)co2_val : (int16_t)co2_val;

            bool temp_neg = (data[6] != 0x00);
            uint8_t temp_val = data[7];
            s_calibration[sensor_idx].temp_offset = temp_neg ? -(int16_t)temp_val : (int16_t)temp_val;

            bool hum_neg = (data[8] != 0x00);
            uint8_t hum_val = data[9];
            s_calibration[sensor_idx].hum_offset = hum_neg ? -(int16_t)hum_val : (int16_t)hum_val;

            s_calibration[sensor_idx].use_fahrenheit = (data[10] != 0x00);
            s_calibration[sensor_idx].valid = true;
            xSemaphoreGive(s_ble_mutex);
            ESP_LOGI(TAG, "Sensor %d calibration: CO2=%d, Temp=%d, Hum=%d, Unit=%s",
                     sensor_idx, s_calibration[sensor_idx].co2_offset,
                     s_calibration[sensor_idx].temp_offset,
                     s_calibration[sensor_idx].hum_offset,
                     s_calibration[sensor_idx].use_fahrenheit ? "F" : "C");
            if (s_settings_request_mode) {
                s_settings_responses_received++;
            }
            return false;
        }

        // Real-time data (cmd 0x01)
        if (cmd_id != 0x01 || len < 13) {
            if (cmd_id != 0x01) {
                ESP_LOGD(TAG, "Ignoring cmd 0x%02X", cmd_id);
            }
            return false;
        }
        ESP_LOGI(TAG, "Parsing real-time data response (cmd=0x01)");

        xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
        inkbird_reading_t *reading = &s_readings[sensor_idx];

        // Temperature (bytes 5-6, with sign in byte 4 lower nibble)
        bool is_negative = (data[4] & 0x0F) != 0;
        uint16_t temp_raw = ((uint16_t)data[5] << 8) | data[6];
        reading->temperature = is_negative ? -(int16_t)temp_raw : (int16_t)temp_raw;

        // Humidity (bytes 7-8)
        reading->humidity = ((uint16_t)data[7] << 8) | data[8];

        // CO2 (bytes 9-10)
        reading->co2_ppm = ((uint16_t)data[9] << 8) | data[10];

        // Pressure (bytes 11-12)
        reading->pressure = ((uint16_t)data[11] << 8) | data[12];

        // Update metadata
        reading->timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
        reading->valid = true;
        reading->stale = false;

        xSemaphoreGive(s_ble_mutex);

        ESP_LOGI(TAG, "=== SENSOR DATA ===");
        ESP_LOGI(TAG, "  Sensor %d [%s]:", sensor_idx, s_active_sensors[sensor_idx].name);
        ESP_LOGI(TAG, "  CO2: %u ppm", reading->co2_ppm);
        ESP_LOGI(TAG, "  Temperature: %.1f C", reading->temperature / 10.0f);
        ESP_LOGI(TAG, "  Humidity: %.1f%%", reading->humidity / 10.0f);
        ESP_LOGI(TAG, "  Pressure: %u hPa", reading->pressure);

        sensor_data_set_activity_status(sensor_idx, NULL);  // Clear status on success
        return true;
    }

    // Unknown format
    if (len >= 4 && data[0] == 0x55 && data[1] == 0xAA) {
        ESP_LOGD(TAG, "Ignoring response with cmd=0x%02X", data[2]);
        return false;
    }

    ESP_LOGW(TAG, "Unknown data format: len=%d", len);
    if (len >= 4) {
        ESP_LOGW(TAG, "  First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X",
                 data[0], data[1], data[2], data[3]);
    }
    return false;
}

// ============================================================================
// Read Task (Sequential with Peer Management) - NimBLE version
// ============================================================================

void inkbird_read_task(void *arg)
{
    ESP_LOGI(TAG, "Read task started (sequential mode with NimBLE)");

    while (s_running) {
        ESP_LOGI(TAG, "=== Starting read cycle for %d sensors ===", s_active_sensor_count);

        // Process each sensor sequentially to avoid BLE memory exhaustion
        for (int i = 0; i < s_active_sensor_count && s_running; i++) {
            // Skip if sensor not enabled
            if (!s_active_sensors[i].enabled) {
                continue;
            }

            // Skip if in skip period after failures
            if (s_skip_cycles[i] > 0) {
                s_skip_cycles[i]--;
                ESP_LOGD(TAG, "Skipping sensor %d, %d cycles remaining", i, s_skip_cycles[i]);
                continue;
            }

            // Clear peer list and create a single peer for this sensor
            s_peer_count = 0;
            inkbird_peer_t *peer = peer_add(i);
            if (peer == NULL) {
                ESP_LOGE(TAG, "Failed to create peer for sensor %d", i);
                continue;
            }

            // Drain stale semaphore signals
            while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

            // Update legacy globals
            s_current_sensor_index = i;
            s_data_received = false;
            s_connected = false;

            ESP_LOGI(TAG, "Connecting to sensor %d (%s): %02X:%02X:%02X:%02X:%02X:%02X",
                     peer->sensor_idx, s_active_sensors[peer->sensor_idx].name,
                     peer->remote_addr.val[5], peer->remote_addr.val[4],
                     peer->remote_addr.val[3], peer->remote_addr.val[2],
                     peer->remote_addr.val[1], peer->remote_addr.val[0]);

            // Start connection
            inkbird_start_connect(peer);

            // Wait for data or timeout
            BaseType_t got_data = xSemaphoreTake(s_read_complete_sem,
                                                  pdMS_TO_TICKS(INKBIRD_CONNECT_TIMEOUT_MS));

            // Check result
            if (got_data == pdTRUE && peer->data_received) {
                ESP_LOGI(TAG, "  Sensor %d (%s): OK", i, s_active_sensors[i].name);
                s_failure_count[i] = 0;
            } else {
                ESP_LOGW(TAG, "  Sensor %d (%s): No data (timeout)", i, s_active_sensors[i].name);
                s_failure_count[i]++;

                if (s_failure_count[i] >= INKBIRD_MAX_FAILURES) {
                    ESP_LOGW(TAG, "Sensor %d: %d failures, skipping %d cycles",
                             i, s_failure_count[i], INKBIRD_SKIP_CYCLES_ON_FAILURE);
                    s_skip_cycles[i] = INKBIRD_SKIP_CYCLES_ON_FAILURE;
                    s_failure_count[i] = 0;
                }
            }

            // Disconnect
            if (peer->connected && peer->conn_handle != INVALID_CONN_HANDLE) {
                ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            // Small delay between sensors
            vTaskDelay(pdMS_TO_TICKS(INKBIRD_INTER_SENSOR_DELAY_MS));
        }

        // Clear peer list after cycle
        s_peer_count = 0;

        // Wait until next read cycle
        if (s_running) {
            ESP_LOGI(TAG, "Read cycle complete, next in %d seconds",
                     INKBIRD_READ_INTERVAL_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(INKBIRD_READ_INTERVAL_MS));
        }
    }

    // Cleanup
    s_peer_count = 0;
    ESP_LOGI(TAG, "Read task exiting");
    vTaskDelete(NULL);
}

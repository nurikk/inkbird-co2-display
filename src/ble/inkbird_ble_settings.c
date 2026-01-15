/**
 * @file inkbird_ble_settings.c
 * @brief Settings synchronization for Inkbird sensors
 *
 * Handles reading and writing sensor configuration:
 * - CO2 mode settings (display mode, custom mode, auto calibration)
 * - CO2 thresholds (normal and plant mode)
 * - Alarm settings (enabled, mode, threshold)
 * - Calibration offsets (CO2, temperature, humidity)
 */

#include "inkbird_ble_internal.h"

static const char *TAG = "inkbird_settings";

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Calculate checksum for command packet
 */
uint8_t inkbird_calc_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(sum & 0xFF);
}

/**
 * @brief Helper to send a command and wait for acknowledgment
 */
esp_err_t inkbird_send_settings_command(uint8_t sensor_idx, const uint8_t *cmd, size_t len, uint32_t timeout_ms)
{
    if (!s_ble_initialized || sensor_idx >= s_active_sensor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    // Close any existing connection
    if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

    s_current_sensor_index = sensor_idx;
    s_data_received = false;
    s_connected = false;
    memcpy(s_target_bda, s_active_sensors[sensor_idx].mac, 6);

    esp_err_t ret = esp_ble_gattc_open(s_gattc_if, s_target_bda, BLE_ADDR_TYPE_PUBLIC, true);
    if (ret != ESP_OK) {
        ret = esp_ble_gattc_open(s_gattc_if, s_target_bda, BLE_ADDR_TYPE_RANDOM, true);
    }

    if (ret != ESP_OK) {
        return ret;
    }

    // Wait for connection
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem, pdMS_TO_TICKS(timeout_ms));
    if (got_sem != pdTRUE || !s_connected || s_cmd_char_handle == 0) {
        if (s_connected) {
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
        } else {
            esp_ble_gap_disconnect(s_target_bda);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    // Send command
    ESP_LOGI(TAG, "Sending settings command:");
    ESP_LOG_BUFFER_HEX(TAG, cmd, len);

    ret = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_cmd_char_handle,
        len, (uint8_t *)cmd, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

    vTaskDelay(pdMS_TO_TICKS(500));

    // Disconnect
    if (s_connected) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return ret;
}

// ============================================================================
// Settings Public API
// ============================================================================

inkbird_device_settings_t inkbird_ble_get_settings(uint8_t index)
{
    inkbird_device_settings_t settings = {0};
    if (index >= INKBIRD_SENSOR_COUNT) {
        return settings;
    }

    if (s_ble_mutex != NULL) {
        xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
        settings.co2_settings = s_co2_settings[index];
        settings.thresholds = s_thresholds[index];
        settings.alarm = s_alarm_settings[index];
        settings.calibration = s_calibration[index];
        xSemaphoreGive(s_ble_mutex);
    }
    return settings;
}

esp_err_t inkbird_ble_request_settings(uint8_t sensor_idx, uint32_t timeout_ms)
{
    if (!s_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sensor_idx >= s_active_sensor_count) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Requesting settings from sensor %d...", sensor_idx);

    // Close any existing connection
    if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Drain stale semaphore signals
    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

    // Set up for settings request
    s_current_sensor_index = sensor_idx;
    s_settings_request_mode = true;
    s_settings_responses_received = 0;
    s_data_received = false;
    s_connected = false;
    memcpy(s_target_bda, s_active_sensors[sensor_idx].mac, 6);

    // Connect
    esp_err_t ret = esp_ble_gattc_open(s_gattc_if, s_target_bda, BLE_ADDR_TYPE_PUBLIC, true);
    if (ret != ESP_OK) {
        ret = esp_ble_gattc_open(s_gattc_if, s_target_bda, BLE_ADDR_TYPE_RANDOM, true);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect: %s", esp_err_to_name(ret));
        s_settings_request_mode = false;
        return ret;
    }

    // Wait for connection and CCCD setup
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem, pdMS_TO_TICKS(timeout_ms / 2));
    if (got_sem != pdTRUE || !s_connected) {
        ESP_LOGE(TAG, "Connection timeout");
        s_settings_request_mode = false;
        if (s_connected) {
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
        } else {
            esp_ble_gap_disconnect(s_target_bda);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    // Send all settings query commands
    if (s_cmd_char_handle != 0) {
        ESP_LOGI(TAG, "Sending settings query commands...");

        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_cmd_char_handle,
            sizeof(CMD_CO2_SETTINGS), (uint8_t *)CMD_CO2_SETTINGS,
            ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_cmd_char_handle,
            sizeof(CMD_CO2_THRESHOLDS), (uint8_t *)CMD_CO2_THRESHOLDS,
            ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_cmd_char_handle,
            sizeof(CMD_CO2_ALARM), (uint8_t *)CMD_CO2_ALARM,
            ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_cmd_char_handle,
            sizeof(CMD_CALIBRATION), (uint8_t *)CMD_CALIBRATION,
            ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    }

    // Wait for responses
    vTaskDelay(pdMS_TO_TICKS(timeout_ms / 2));

    ESP_LOGI(TAG, "Settings responses received: %d/4", s_settings_responses_received);

    // Disconnect
    if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_settings_request_mode = false;
    s_connected = false;

    return (s_settings_responses_received > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t inkbird_ble_set_thresholds(uint8_t sensor_idx,
                                      uint16_t normal_high, uint16_t normal_low,
                                      uint16_t plant_high, uint16_t plant_low,
                                      bool reset_to_defaults)
{
    // Command 0x03: 55 AA 03 0E [norm_high] [norm_low] [plant_high] [plant_low] [reset] [checksum]
    uint8_t cmd[14];
    cmd[0] = 0x55;
    cmd[1] = 0xAA;
    cmd[2] = 0x03;
    cmd[3] = 0x0E;
    cmd[4] = (normal_high >> 8) & 0xFF;
    cmd[5] = normal_high & 0xFF;
    cmd[6] = (normal_low >> 8) & 0xFF;
    cmd[7] = normal_low & 0xFF;
    cmd[8] = (plant_high >> 8) & 0xFF;
    cmd[9] = plant_high & 0xFF;
    cmd[10] = (plant_low >> 8) & 0xFF;
    cmd[11] = plant_low & 0xFF;
    cmd[12] = reset_to_defaults ? 0x01 : 0x00;
    cmd[13] = inkbird_calc_checksum(cmd, 13);

    return inkbird_send_settings_command(sensor_idx, cmd, sizeof(cmd), 10000);
}

esp_err_t inkbird_ble_set_alarm(uint8_t sensor_idx,
                                 bool enabled, uint8_t alarm_mode, uint16_t alarm_value)
{
    // Command 0x04: 55 AA 04 09 [enabled] [mode] [value_hi] [value_lo] [checksum]
    uint8_t cmd[9];
    cmd[0] = 0x55;
    cmd[1] = 0xAA;
    cmd[2] = 0x04;
    cmd[3] = 0x09;
    cmd[4] = enabled ? 0x01 : 0x00;
    cmd[5] = alarm_mode;
    cmd[6] = (alarm_value >> 8) & 0xFF;
    cmd[7] = alarm_value & 0xFF;
    cmd[8] = inkbird_calc_checksum(cmd, 8);

    return inkbird_send_settings_command(sensor_idx, cmd, sizeof(cmd), 10000);
}

esp_err_t inkbird_ble_set_calibration(uint8_t sensor_idx,
                                       int16_t co2_offset, int16_t temp_offset,
                                       int16_t hum_offset, bool use_fahrenheit)
{
    // Command 0x05: 55 AA 05 0C [co2_sign] [co2_val] [temp_sign] [temp_val] [hum_sign] [hum_val] [unit] [checksum]
    uint8_t cmd[12];
    cmd[0] = 0x55;
    cmd[1] = 0xAA;
    cmd[2] = 0x05;
    cmd[3] = 0x0C;
    cmd[4] = (co2_offset < 0) ? 0x01 : 0x00;
    cmd[5] = (uint8_t)(co2_offset < 0 ? -co2_offset : co2_offset);
    cmd[6] = (temp_offset < 0) ? 0x01 : 0x00;
    cmd[7] = (uint8_t)(temp_offset < 0 ? -temp_offset : temp_offset);
    cmd[8] = (hum_offset < 0) ? 0x01 : 0x00;
    cmd[9] = (uint8_t)(hum_offset < 0 ? -hum_offset : hum_offset);
    cmd[10] = use_fahrenheit ? 0x01 : 0x00;
    cmd[11] = inkbird_calc_checksum(cmd, 11);

    return inkbird_send_settings_command(sensor_idx, cmd, sizeof(cmd), 10000);
}

esp_err_t inkbird_ble_set_co2_mode(uint8_t sensor_idx,
                                    uint8_t display_mode, bool use_custom,
                                    bool auto_calibration)
{
    // Command 0x02: 55 AA 02 0B [mode] [custom] [auto] [manual=0] [cal_hi=0] [cal_lo=0] [checksum]
    uint8_t cmd[11];
    cmd[0] = 0x55;
    cmd[1] = 0xAA;
    cmd[2] = 0x02;
    cmd[3] = 0x0B;
    cmd[4] = display_mode;
    cmd[5] = use_custom ? 0x01 : 0x00;
    cmd[6] = auto_calibration ? 0x01 : 0x00;
    cmd[7] = 0x00;
    cmd[8] = 0x00;
    cmd[9] = 0x00;
    cmd[10] = inkbird_calc_checksum(cmd, 10);

    return inkbird_send_settings_command(sensor_idx, cmd, sizeof(cmd), 10000);
}

esp_err_t inkbird_ble_calibrate_co2(uint8_t sensor_idx, uint16_t cal_value)
{
    // Command 0x02 with manual_mode=1: Start calibration
    uint8_t cmd[11];
    cmd[0] = 0x55;
    cmd[1] = 0xAA;
    cmd[2] = 0x02;
    cmd[3] = 0x0B;
    cmd[4] = 0x00;
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x01;  // Manual mode = calibrating
    cmd[8] = (cal_value >> 8) & 0xFF;
    cmd[9] = cal_value & 0xFF;
    cmd[10] = inkbird_calc_checksum(cmd, 10);

    return inkbird_send_settings_command(sensor_idx, cmd, sizeof(cmd), 10000);
}

esp_err_t inkbird_ble_reset_co2(uint8_t sensor_idx)
{
    // Command 0x02 with manual_mode=4: Reset CO2 sensor
    uint8_t cmd[11];
    cmd[0] = 0x55;
    cmd[1] = 0xAA;
    cmd[2] = 0x02;
    cmd[3] = 0x0B;
    cmd[4] = 0x00;
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x04;  // Manual mode = reset
    cmd[8] = 0x00;
    cmd[9] = 0x00;
    cmd[10] = inkbird_calc_checksum(cmd, 10);

    return inkbird_send_settings_command(sensor_idx, cmd, sizeof(cmd), 10000);
}

/**
 * @file inkbird_ble.c
 * @brief Core BLE module for Inkbird IAM-T1 CO2 sensors
 *
 * This is the main entry point for the Inkbird BLE module. It provides:
 * - BLE stack initialization and configuration
 * - Shared state variable definitions
 * - Public API wrappers for initialization, start/stop, and sensor access
 *
 * The module is split across multiple files for maintainability:
 * - inkbird_ble.c: Core initialization and public API (this file)
 * - inkbird_ble_protocol.c: GAP/GATT event handlers
 * - inkbird_ble_data.c: Data parsing and read task
 * - inkbird_ble_history.c: Historical data download
 * - inkbird_ble_settings.c: Settings synchronization
 */

#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "nvs_flash.h"

#include "inkbird_ble.h"
#include "inkbird_ble_internal.h"

static const char *TAG = "inkbird_ble";

// ============================================================================
// Command Packets (from APK reverse engineering)
// ============================================================================

const uint8_t CMD_REALTIME_DATA[] = {0x55, 0xAA, 0x09, 0x06, 0x01, 0x0F};
const uint8_t CMD_PAIRING[] = {0x55, 0xAA, 0x08, 0x06, 0x01, 0x0E};
const uint8_t CMD_CO2_SETTINGS[] = {0x55, 0xAA, 0x02, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C};
const uint8_t CMD_CO2_THRESHOLDS[] = {0x55, 0xAA, 0x03, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
const uint8_t CMD_CO2_ALARM[] = {0x55, 0xAA, 0x04, 0x09, 0x00, 0x00, 0x00, 0x00, 0x0D};
const uint8_t CMD_CALIBRATION[] = {0x55, 0xAA, 0x05, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12};
const uint8_t CMD_HISTORY_START[] = {0x55, 0xAA, 0x07, 0x06, 0x00, 0x0C};
const uint8_t CMD_HISTORY_STOP[] = {0x55, 0xAA, 0x07, 0x06, 0x01, 0x0D};

// ============================================================================
// Shared State Variables
// ============================================================================

// Sensor readings and settings storage
inkbird_reading_t s_readings[INKBIRD_SENSOR_COUNT];
inkbird_co2_thresholds_t s_thresholds[INKBIRD_SENSOR_COUNT];
inkbird_co2_settings_t s_co2_settings[INKBIRD_SENSOR_COUNT];
inkbird_alarm_settings_t s_alarm_settings[INKBIRD_SENSOR_COUNT];
inkbird_calibration_t s_calibration[INKBIRD_SENSOR_COUNT];

// Settings request mode
bool s_settings_request_mode = false;
uint8_t s_settings_responses_received = 0;

// Failure tracking
uint8_t s_failure_count[INKBIRD_SENSOR_COUNT];
uint8_t s_skip_cycles[INKBIRD_SENSOR_COUNT];

// Runtime sensor registry
inkbird_sensor_config_t s_active_sensors[INKBIRD_SENSOR_COUNT];
uint8_t s_active_sensor_count = 0;

// Discovered sensors
inkbird_discovered_t s_discovered[INKBIRD_MAX_DISCOVERED];
uint8_t s_discovered_count = 0;

// BLE state
bool s_ble_initialized = false;
bool s_running = false;
bool s_scanning = false;
bool s_connected = false;
uint16_t s_conn_id = 0;
uint8_t s_current_sensor_index = 0;
esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;

// Peer management (multi-connection)
inkbird_peer_t s_peers[MAX_PEERS];
uint8_t s_peer_count = 0;

// GATT handles (legacy - kept for history/settings compatibility)
uint16_t s_service_start_handle = 0;
uint16_t s_service_end_handle = 0;
uint16_t s_data_char_handle = 0;
uint16_t s_cmd_char_handle = 0;
uint16_t s_cccd_handle = 0;

// Synchronization
SemaphoreHandle_t s_ble_mutex = NULL;
SemaphoreHandle_t s_read_complete_sem = NULL;

// Task handle
TaskHandle_t s_read_task_handle = NULL;

// Receive buffer
uint8_t s_recv_data[32];
size_t s_recv_len = 0;
bool s_data_received = false;

// Target address
esp_bd_addr_t s_target_bda;

// History download state
inkbird_history_state_t s_history_state = INKBIRD_HISTORY_IDLE;
inkbird_history_record_t *s_history_records = NULL;
uint16_t s_history_max_records = 0;
uint16_t s_history_expected_count = 0;
uint16_t s_history_received_count = 0;
uint16_t s_history_stored_count = 0;
bool s_history_got_count = false;
uint8_t s_history_buffer[256];
size_t s_history_buffer_len = 0;
SemaphoreHandle_t s_history_complete_sem = NULL;
uint16_t s_history_write_idx = 0;
bool s_history_buffer_wrapped = false;

// ============================================================================
// Peer Manager Functions
// ============================================================================

inkbird_peer_t *peer_find_by_conn_id(uint16_t conn_id)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].conn_id == conn_id) {
            return &s_peers[i];
        }
    }
    return NULL;
}

inkbird_peer_t *peer_find_by_mac(const esp_bd_addr_t bda)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i].remote_bda, bda, 6) == 0) {
            return &s_peers[i];
        }
    }
    return NULL;
}

inkbird_peer_t *peer_add(uint8_t sensor_idx)
{
    if (s_peer_count >= MAX_PEERS) {
        ESP_LOGW(TAG, "Peer list full, cannot add sensor %d", sensor_idx);
        return NULL;
    }

    inkbird_peer_t *peer = &s_peers[s_peer_count++];
    memset(peer, 0, sizeof(*peer));
    peer->sensor_idx = sensor_idx;
    peer->conn_id = INVALID_CONN_ID;
    memcpy(peer->remote_bda, s_active_sensors[sensor_idx].mac, 6);

    ESP_LOGI(TAG, "Added peer %d for sensor %d (%s)",
             s_peer_count - 1, sensor_idx, s_active_sensors[sensor_idx].name);
    return peer;
}

void peer_remove(inkbird_peer_t *peer)
{
    if (peer == NULL) return;

    int idx = peer - s_peers;
    if (idx < 0 || idx >= s_peer_count) return;

    ESP_LOGI(TAG, "Removing peer %d (sensor %d)", idx, peer->sensor_idx);

    if (idx < s_peer_count - 1) {
        memmove(&s_peers[idx], &s_peers[idx + 1],
                (s_peer_count - idx - 1) * sizeof(inkbird_peer_t));
    }
    s_peer_count--;
}

void peer_reset(inkbird_peer_t *peer)
{
    if (peer == NULL) return;

    uint8_t sensor_idx = peer->sensor_idx;
    esp_bd_addr_t bda;
    memcpy(bda, peer->remote_bda, 6);

    memset(peer, 0, sizeof(*peer));
    peer->sensor_idx = sensor_idx;
    peer->conn_id = INVALID_CONN_ID;
    memcpy(peer->remote_bda, bda, 6);
}

uint8_t peer_count_connected(void)
{
    uint8_t count = 0;
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].connected) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// Public API - Initialization
// ============================================================================

esp_err_t inkbird_ble_init(void)
{
    if (s_ble_initialized) {
        ESP_LOGW(TAG, "BLE already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing BLE (Bluedroid stack) for Inkbird sensors...");

    // Initialize NVS (required for BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize readings and thresholds
    memset(s_readings, 0, sizeof(s_readings));
    memset(s_thresholds, 0, sizeof(s_thresholds));
    memset(s_failure_count, 0, sizeof(s_failure_count));
    memset(s_skip_cycles, 0, sizeof(s_skip_cycles));

    // Initialize peer array
    memset(s_peers, 0, sizeof(s_peers));
    s_peer_count = 0;

    // Initialize active sensors registry
    memset(s_active_sensors, 0, sizeof(s_active_sensors));
    s_active_sensor_count = 0;
    for (int i = 0; i < INKBIRD_SENSOR_COUNT; i++) {
        if (INKBIRD_SENSORS[i].enabled) {
            s_active_sensors[s_active_sensor_count] = INKBIRD_SENSORS[i];
            ESP_LOGI(TAG, "Configured sensor %d: %s", s_active_sensor_count, INKBIRD_SENSORS[i].name);
            s_active_sensor_count++;
        }
    }
    ESP_LOGI(TAG, "Loaded %d configured sensor(s)", s_active_sensor_count);

    // Initialize thresholds with protocol defaults
    for (int i = 0; i < INKBIRD_SENSOR_COUNT; i++) {
        s_thresholds[i].normal_low_ppm = 420;
        s_thresholds[i].normal_high_ppm = 2000;
        s_thresholds[i].plant_low_ppm = 340;
        s_thresholds[i].plant_high_ppm = 5000;
        s_thresholds[i].use_custom = false;
        s_thresholds[i].settings_valid = false;
        s_thresholds[i].thresholds_valid = true;
    }
    ESP_LOGI(TAG, "Initialized with protocol default thresholds: normal=420-2000, plant=340-5000 ppm");

    // Create synchronization primitives
    s_ble_mutex = xSemaphoreCreateMutex();
    if (s_ble_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_read_complete_sem = xSemaphoreCreateBinary();
    if (s_read_complete_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_ERR_NO_MEM;
    }

    // Release BT classic memory
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Disable brownout detector during RF calibration
    ESP_LOGI(TAG, "Free heap before BLE: %lu bytes", esp_get_free_heap_size());
    ESP_LOGW(TAG, "Disabling brownout detector for RF calibration...");
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init BT controller: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable BT controller: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Bluedroid: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable Bluedroid: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register callbacks (implemented in inkbird_ble_protocol.c)
    ret = esp_ble_gap_register_callback(inkbird_gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GAP callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gattc_register_callback(inkbird_gattc_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GATTC callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gattc_app_register(GATTC_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GATTC app: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set MTU
    ret = esp_ble_gatt_set_local_mtu(247);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set local MTU: %s", esp_err_to_name(ret));
    }

    s_ble_initialized = true;
    ESP_LOGI(TAG, "BLE initialized successfully (Bluedroid stack)");

    return ESP_OK;
}

// ============================================================================
// Public API - Start/Stop
// ============================================================================

esp_err_t inkbird_ble_start(void)
{
    if (!s_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGW(TAG, "BLE reading already running");
        return ESP_OK;
    }

    if (s_active_sensor_count == 0) {
        ESP_LOGW(TAG, "No active sensors");
        ESP_LOGW(TAG, "Run inkbird_ble_discover() then inkbird_ble_register_discovered()");
    } else {
        for (int i = 0; i < s_active_sensor_count; i++) {
            ESP_LOGI(TAG, "Active sensor %d: %s", i, s_active_sensors[i].name);
        }
    }

    s_running = true;

    // Create read task (implemented in inkbird_ble_data.c)
    BaseType_t xret = xTaskCreatePinnedToCore(
        inkbird_read_task,
        "inkbird_read",
        4096,
        NULL,
        5,
        &s_read_task_handle,
        BLE_TASK_CORE
    );

    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create read task");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BLE reading started");
    return ESP_OK;
}

esp_err_t inkbird_ble_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;

    // Wait for task to exit
    if (s_read_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_read_task_handle = NULL;
    }

    // Disconnect if connected
    if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    }

    ESP_LOGI(TAG, "BLE reading stopped");
    return ESP_OK;
}

// ============================================================================
// Public API - Sensor Reading
// ============================================================================

inkbird_reading_t inkbird_ble_get_reading(uint8_t index)
{
    inkbird_reading_t reading = {0};

    if (index >= INKBIRD_SENSOR_COUNT) {
        return reading;
    }

    xSemaphoreTake(s_ble_mutex, portMAX_DELAY);

    reading = s_readings[index];

    // Check if data is stale
    if (reading.valid) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t age = now - reading.timestamp;
        reading.stale = (age > INKBIRD_DATA_STALE_MS);
    }

    xSemaphoreGive(s_ble_mutex);

    return reading;
}

bool inkbird_ble_is_connected(uint8_t index)
{
    if (index >= INKBIRD_SENSOR_COUNT) {
        return false;
    }
    return s_connected && (s_current_sensor_index == index);
}

esp_err_t inkbird_ble_read_sensor_once(uint8_t sensor_idx,
                                        uint32_t timeout_ms,
                                        inkbird_reading_t *out_reading)
{
    if (!s_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sensor_idx >= s_active_sensor_count) {
        ESP_LOGE(TAG, "Invalid sensor index: %d (active count: %d)", sensor_idx, s_active_sensor_count);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_active_sensors[sensor_idx].enabled) {
        ESP_LOGE(TAG, "Sensor %d is not enabled", sensor_idx);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "One-shot read: sensor %d (%s), timeout %lu ms",
             sensor_idx, s_active_sensors[sensor_idx].name, timeout_ms);

    // Close any existing connections and clear peer list
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].connected && s_gattc_if != ESP_GATT_IF_NONE) {
            esp_ble_gattc_close(s_gattc_if, s_peers[i].conn_id);
        }
    }
    if (s_peer_count > 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_peer_count = 0;

    // Drain stale semaphore signals
    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

    // Create a peer for this sensor
    inkbird_peer_t *peer = peer_add(sensor_idx);
    if (peer == NULL) {
        ESP_LOGE(TAG, "Failed to create peer for sensor %d", sensor_idx);
        return ESP_ERR_NO_MEM;
    }

    // Set legacy globals for compatibility
    s_current_sensor_index = sensor_idx;
    s_data_received = false;
    s_connected = false;
    memcpy(s_target_bda, s_active_sensors[sensor_idx].mac, 6);

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             peer->remote_bda[0], peer->remote_bda[1], peer->remote_bda[2],
             peer->remote_bda[3], peer->remote_bda[4], peer->remote_bda[5]);

    // Open connection
    esp_err_t ret = esp_ble_gattc_open(
        s_gattc_if, peer->remote_bda,
        BLE_ADDR_TYPE_PUBLIC, true);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GATTC open failed with public addr: %s, trying random",
                 esp_err_to_name(ret));
        ret = esp_ble_gattc_open(
            s_gattc_if, peer->remote_bda,
            BLE_ADDR_TYPE_RANDOM, true);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initiate connection: %s", esp_err_to_name(ret));
        s_peer_count = 0;  // Clean up peer
        return ret;
    }

    // Wait for data or timeout
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem,
                                         pdMS_TO_TICKS(timeout_ms));

    esp_err_t result;
    // Check peer data_received flag (more reliable than legacy s_data_received)
    bool data_ok = (got_sem == pdTRUE && peer != NULL && peer->data_received);

    if (data_ok) {
        ESP_LOGI(TAG, "One-shot read successful for sensor %d", sensor_idx);

        if (out_reading != NULL) {
            xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
            *out_reading = s_readings[sensor_idx];
            xSemaphoreGive(s_ble_mutex);
        }
        result = ESP_OK;
    } else {
        ESP_LOGW(TAG, "One-shot read timeout/failed for sensor %d", sensor_idx);
        result = ESP_ERR_TIMEOUT;
    }

    // Disconnect using peer state
    if (peer != NULL && peer->connected && s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, peer->conn_id);
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else if (peer != NULL && s_gattc_if != ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "Cancelling pending connection to clean up BLE state");
        esp_ble_gap_disconnect(peer->remote_bda);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Clean up peer
    s_peer_count = 0;
    s_connected = false;

    return result;
}

// ============================================================================
// Public API - Discovery
// ============================================================================

esp_err_t inkbird_ble_discover(void)
{
    if (!s_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting Inkbird Sensor Discovery");
    ESP_LOGI(TAG, "  Looking for service UUID: 0x%04X", INKBIRD_SERVICE_UUID);
    ESP_LOGI(TAG, "  Scan duration: %d seconds", INKBIRD_SCAN_DURATION_SEC);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    // Clear previous discoveries
    s_discovered_count = 0;
    memset(s_discovered, 0, sizeof(s_discovered));

    // Start scanning
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set scan params: %s", esp_err_to_name(ret));
        return ret;
    }

    s_scanning = true;
    vTaskDelay(pdMS_TO_TICKS((INKBIRD_SCAN_DURATION_SEC + 2) * 1000));
    s_scanning = false;

    esp_ble_gap_stop_scanning();

    // Log results
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Discovery Complete: %d sensor(s) found", s_discovered_count);
    ESP_LOGI(TAG, "========================================");

    if (s_discovered_count == 0) {
        ESP_LOGW(TAG, "No Inkbird sensors found!");
        ESP_LOGW(TAG, "Make sure sensors are powered on and nearby");
    } else {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Copy these MAC addresses to inkbird_config.h:");
        ESP_LOGI(TAG, "");

        for (int i = 0; i < s_discovered_count; i++) {
            ESP_LOGI(TAG, "[%d] MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X} (addr_type=%d)",
                     i,
                     s_discovered[i].mac[0], s_discovered[i].mac[1],
                     s_discovered[i].mac[2], s_discovered[i].mac[3],
                     s_discovered[i].mac[4], s_discovered[i].mac[5],
                     s_discovered[i].addr_type);
            ESP_LOGI(TAG, "    RSSI: %d dBm, Name: %s",
                     s_discovered[i].rssi,
                     s_discovered[i].name[0] ? s_discovered[i].name : "(no name)");
        }

        ESP_LOGI(TAG, "");
    }
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

uint8_t inkbird_ble_get_discovered_count(void)
{
    return s_discovered_count;
}

esp_err_t inkbird_ble_get_discovered(uint8_t index, inkbird_discovered_t *out_info)
{
    if (index >= s_discovered_count || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_info = s_discovered[index];
    return ESP_OK;
}

void inkbird_ble_register_discovered(void)
{
    ESP_LOGI(TAG, "Registering discovered sensors...");

    for (int i = 0; i < s_discovered_count && s_active_sensor_count < INKBIRD_SENSOR_COUNT; i++) {
        bool already_known = false;
        for (int j = 0; j < s_active_sensor_count; j++) {
            if (memcmp(s_active_sensors[j].mac, s_discovered[i].mac, 6) == 0) {
                already_known = true;
                ESP_LOGI(TAG, "  Sensor %02X:%02X:%02X:%02X:%02X:%02X already configured as '%s'",
                         s_discovered[i].mac[0], s_discovered[i].mac[1],
                         s_discovered[i].mac[2], s_discovered[i].mac[3],
                         s_discovered[i].mac[4], s_discovered[i].mac[5],
                         s_active_sensors[j].name);
                break;
            }
        }
        if (!already_known) {
            inkbird_sensor_config_t *slot = &s_active_sensors[s_active_sensor_count];
            memcpy(slot->mac, s_discovered[i].mac, 6);
            if (s_discovered[i].name[0]) {
                strncpy(slot->name, s_discovered[i].name, sizeof(slot->name) - 1);
                slot->name[sizeof(slot->name) - 1] = '\0';
            } else {
                snprintf(slot->name, sizeof(slot->name), "Sensor %d", s_active_sensor_count);
            }
            slot->enabled = true;
            ESP_LOGI(TAG, "  Auto-registered sensor %d: %02X:%02X:%02X:%02X:%02X:%02X as '%s'",
                     s_active_sensor_count,
                     slot->mac[0], slot->mac[1], slot->mac[2],
                     slot->mac[3], slot->mac[4], slot->mac[5],
                     slot->name);
            s_active_sensor_count++;
        }
    }

    ESP_LOGI(TAG, "Total active sensors: %d", s_active_sensor_count);
}

// ============================================================================
// Public API - Sensor Info
// ============================================================================

uint8_t inkbird_ble_get_active_count(void)
{
    return s_active_sensor_count;
}

const char *inkbird_ble_get_sensor_name(uint8_t index)
{
    if (index >= s_active_sensor_count) {
        return "Unknown";
    }
    return s_active_sensors[index].name;
}

bool inkbird_ble_is_sensor_enabled(uint8_t index)
{
    if (index >= s_active_sensor_count) {
        return false;
    }
    return s_active_sensors[index].enabled;
}

inkbird_co2_thresholds_t inkbird_ble_get_thresholds(uint8_t index)
{
    inkbird_co2_thresholds_t thresholds = {0};
    if (index >= INKBIRD_SENSOR_COUNT) {
        return thresholds;
    }
    if (s_ble_mutex != NULL) {
        xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
        thresholds = s_thresholds[index];
        xSemaphoreGive(s_ble_mutex);
    }
    return thresholds;
}

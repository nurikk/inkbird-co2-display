/**
 * @file inkbird_ble.c
 * @brief Core BLE module for Inkbird IAM-T1 CO2 sensors (NimBLE stack)
 *
 * This is the main entry point for the Inkbird BLE module. It provides:
 * - NimBLE stack initialization and configuration
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
bool s_ble_synced = false;
bool s_running = false;
bool s_scanning = false;
bool s_connected = false;
uint16_t s_conn_handle = INVALID_CONN_HANDLE;
uint8_t s_current_sensor_index = 0;
uint8_t s_own_addr_type = 0;

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
ble_addr_t s_target_addr;

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
bool s_history_setup_mode = false;
uint8_t s_history_downsample_rate = 1;
uint16_t s_history_downsample_counter = 0;

// ============================================================================
// NimBLE Callbacks
// ============================================================================

static void inkbird_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

static void inkbird_on_sync(void)
{
    int rc;

    ESP_LOGI(TAG, "NimBLE host synced");

    // Determine best address type
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: %d", rc);
        return;
    }

    uint8_t addr[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Device address: %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], s_own_addr_type);

    s_ble_synced = true;
}

void inkbird_nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ============================================================================
// Peer Manager Functions
// ============================================================================

inkbird_peer_t *peer_find_by_conn_handle(uint16_t conn_handle)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].conn_handle == conn_handle) {
            return &s_peers[i];
        }
    }
    return NULL;
}

inkbird_peer_t *peer_find_by_addr(const ble_addr_t *addr)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(&s_peers[i].remote_addr, addr, sizeof(ble_addr_t)) == 0) {
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
    peer->conn_handle = INVALID_CONN_HANDLE;

    // Copy MAC address with byte reversal
    // Config stores MACs in big-endian (human-readable: AA:BB:CC:DD:EE:FF = {0xAA,0xBB,...})
    // NimBLE uses little-endian in ble_addr_t.val (val[0]=LSB)
    peer->remote_addr.type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) {
        peer->remote_addr.val[i] = s_active_sensors[sensor_idx].mac[5 - i];
    }

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
    ble_addr_t addr;
    memcpy(&addr, &peer->remote_addr, sizeof(addr));

    memset(peer, 0, sizeof(*peer));
    peer->sensor_idx = sensor_idx;
    peer->conn_handle = INVALID_CONN_HANDLE;
    memcpy(&peer->remote_addr, &addr, sizeof(addr));
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

    ESP_LOGI(TAG, "Initializing BLE (NimBLE stack) for Inkbird sensors...");

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

    // Disable brownout detector during RF calibration
    ESP_LOGI(TAG, "Free heap before BLE: %lu bytes", esp_get_free_heap_size());
    ESP_LOGW(TAG, "Disabling brownout detector for RF calibration...");
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize NimBLE port
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE port: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure NimBLE host
    ble_hs_cfg.reset_cb = inkbird_on_reset;
    ble_hs_cfg.sync_cb = inkbird_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Initialize GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Set device name
    ble_svc_gap_device_name_set("CO2Display");

    // Initialize NimBLE host configuration store
    ble_store_config_init();

    // Start NimBLE host task
    nimble_port_freertos_init(inkbird_nimble_host_task);

    // Wait for host to sync
    int timeout = 50;  // 5 seconds
    while (!s_ble_synced && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }

    if (!s_ble_synced) {
        ESP_LOGE(TAG, "NimBLE host failed to sync");
        return ESP_FAIL;
    }

    s_ble_initialized = true;
    ESP_LOGI(TAG, "BLE initialized successfully (NimBLE stack)");

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

    // Disconnect all peers
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].connected && s_peers[i].conn_handle != INVALID_CONN_HANDLE) {
            ble_gap_terminate(s_peers[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
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

    if (!s_ble_synced) {
        ESP_LOGE(TAG, "BLE not synced");
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
        if (s_peers[i].connected && s_peers[i].conn_handle != INVALID_CONN_HANDLE) {
            ble_gap_terminate(s_peers[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
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
    memcpy(&s_target_addr, &peer->remote_addr, sizeof(s_target_addr));

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             peer->remote_addr.val[5], peer->remote_addr.val[4], peer->remote_addr.val[3],
             peer->remote_addr.val[2], peer->remote_addr.val[1], peer->remote_addr.val[0]);

    // Start connection
    inkbird_start_connect(peer);

    // Wait for data or timeout
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem,
                                         pdMS_TO_TICKS(timeout_ms));

    esp_err_t result;
    // Check peer data_received flag
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

    // Disconnect
    if (peer != NULL && peer->connected && peer->conn_handle != INVALID_CONN_HANDLE) {
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    if (!s_ble_synced) {
        ESP_LOGE(TAG, "BLE not synced");
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
    inkbird_start_scan();

    s_scanning = true;
    vTaskDelay(pdMS_TO_TICKS((INKBIRD_SCAN_DURATION_SEC + 2) * 1000));
    s_scanning = false;

    inkbird_stop_scan();

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
            // Compare with byte reversal: config is big-endian, discovered is little-endian
            bool match = true;
            for (int k = 0; k < 6; k++) {
                if (s_active_sensors[j].mac[k] != s_discovered[i].mac[5 - k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                already_known = true;
                ESP_LOGI(TAG, "  Sensor %02X:%02X:%02X:%02X:%02X:%02X already configured as '%s'",
                         s_discovered[i].mac[5], s_discovered[i].mac[4],
                         s_discovered[i].mac[3], s_discovered[i].mac[2],
                         s_discovered[i].mac[1], s_discovered[i].mac[0],
                         s_active_sensors[j].name);
                break;
            }
        }
        if (!already_known) {
            inkbird_sensor_config_t *slot = &s_active_sensors[s_active_sensor_count];
            // Convert discovered MAC (little-endian) to config format (big-endian)
            for (int k = 0; k < 6; k++) {
                slot->mac[k] = s_discovered[i].mac[5 - k];
            }
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

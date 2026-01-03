/**
 * @file inkbird_ble.c
 * @brief BLE reader implementation for Inkbird IAM-T1 CO2 sensors
 *
 * Uses Bluedroid stack (same as ESPHome) to scan for, connect to, and read
 * data from Inkbird IAM-T1 sensors via GATT notifications.
 */

#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "nvs_flash.h"

#include "inkbird_ble.h"
#include "inkbird_config.h"

static const char *TAG = "inkbird_ble";

// ============================================================================
// Constants
// ============================================================================

#define GATTC_APP_ID            0
#define INVALID_HANDLE          0
#define PROFILE_NUM             1
#define PROFILE_APP_IDX         0

// Inkbird service and characteristic UUIDs (16-bit)
#define INKBIRD_SVC_UUID16      0xFFE0
#define INKBIRD_DATA_UUID16     0xFFE4  // Notifications
#define INKBIRD_CMD_UUID16      0xFFE9  // Write commands

// CCCD UUID for enabling notifications
#define ESP_GATT_UUID_CHAR_CLIENT_CONFIG 0x2902

// History download commands (from APK reverse engineering)
// Command format: 55 AA [cmd] [subcmd] [len] [data...] [checksum]
static const uint8_t CMD_HISTORY_START[] = {0x55, 0xAA, 0x07, 0x06, 0x00, 0x0C};
static const uint8_t CMD_HISTORY_STOP[]  = {0x55, 0xAA, 0x07, 0x06, 0x01, 0x0D};

// History end marker
#define HISTORY_END_MARKER_HIGH 0x66
#define HISTORY_END_MARKER_LOW  0x66

// Maximum history records to store
#define INKBIRD_MAX_HISTORY_RECORDS 1000

// Empty record detection (all 0xFF means uninitialized flash)
#define HISTORY_EMPTY_BYTE 0xFF

// ============================================================================
// State Variables
// ============================================================================

// Sensor readings storage
static inkbird_reading_t s_readings[INKBIRD_SENSOR_COUNT];

// Failure tracking for each sensor
static uint8_t s_failure_count[INKBIRD_SENSOR_COUNT];
static uint8_t s_skip_cycles[INKBIRD_SENSOR_COUNT];

// Discovered sensors storage
static inkbird_discovered_t s_discovered[INKBIRD_MAX_DISCOVERED];
static uint8_t s_discovered_count = 0;

// BLE state
static bool s_ble_initialized = false;
static bool s_running = false;
static bool s_scanning = false;
static bool s_connected = false;
static uint16_t s_conn_id = 0;
static uint8_t s_current_sensor_index = 0;
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;

// GATT handles discovered during connection
static uint16_t s_service_start_handle = 0;
static uint16_t s_service_end_handle = 0;
static uint16_t s_data_char_handle = 0;
static uint16_t s_cmd_char_handle = 0;
static uint16_t s_cccd_handle = 0;

// Synchronization
static SemaphoreHandle_t s_ble_mutex = NULL;
static SemaphoreHandle_t s_read_complete_sem = NULL;

// Task handle
static TaskHandle_t s_read_task_handle = NULL;

// Temporary buffer for received data
static uint8_t s_recv_data[32];
static size_t s_recv_len = 0;
static bool s_data_received = false;

// Current target address
static esp_bd_addr_t s_target_bda;

// ============================================================================
// History Download State
// ============================================================================

static inkbird_history_state_t s_history_state = INKBIRD_HISTORY_IDLE;
static inkbird_history_record_t *s_history_records = NULL;
static uint16_t s_history_max_records = 0;
static uint16_t s_history_expected_count = 0;
static uint16_t s_history_received_count = 0;    // Total valid records parsed
static uint16_t s_history_stored_count = 0;      // Actual count in output buffer
static bool s_history_got_count = false;
static uint8_t s_history_buffer[256];  // Buffer for accumulating partial data
static size_t s_history_buffer_len = 0;
static SemaphoreHandle_t s_history_complete_sem = NULL;

// Circular buffer tracking for getting NEWEST records
// Sensor sends oldest->newest, so we use circular buffer to keep only the last N
static uint16_t s_history_write_idx = 0;      // Next write position (circular)
static bool s_history_buffer_wrapped = false; // True if buffer has wrapped around

// ============================================================================
// Forward Declarations
// ============================================================================

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void read_task(void *arg);
static void parse_inkbird_data(const uint8_t *data, size_t len, uint8_t sensor_idx);
static void parse_history_notification(const uint8_t *data, size_t len);
static bool is_empty_record(const uint8_t *data);
static bool parse_history_record(const uint8_t *data);
static esp_err_t send_history_command(const uint8_t *cmd, size_t len);

// ============================================================================
// Public API Implementation
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

    // Initialize readings
    memset(s_readings, 0, sizeof(s_readings));
    memset(s_failure_count, 0, sizeof(s_failure_count));
    memset(s_skip_cycles, 0, sizeof(s_skip_cycles));

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

    // Release BT classic memory (we only use BLE)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

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

    // Register callbacks
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GAP callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gattc_register_callback(gattc_event_handler);
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

    // Check if any sensors are enabled
    bool any_enabled = false;
    for (int i = 0; i < INKBIRD_SENSOR_COUNT; i++) {
        if (INKBIRD_SENSORS[i].enabled) {
            any_enabled = true;
            ESP_LOGI(TAG, "Sensor %d enabled: %s", i, INKBIRD_SENSORS[i].name);
        }
    }

    if (!any_enabled) {
        ESP_LOGW(TAG, "No sensors enabled in configuration");
        ESP_LOGW(TAG, "Run inkbird_ble_discover() to find sensors");
        ESP_LOGW(TAG, "Then update inkbird_config.h with MAC addresses");
    }

    s_running = true;

    // Create read task
    BaseType_t xret = xTaskCreate(
        read_task,
        "inkbird_read",
        4096,
        NULL,
        5,
        &s_read_task_handle
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

    if (sensor_idx >= INKBIRD_SENSOR_COUNT) {
        ESP_LOGE(TAG, "Invalid sensor index: %d", sensor_idx);
        return ESP_ERR_INVALID_ARG;
    }

    if (!INKBIRD_SENSORS[sensor_idx].enabled) {
        ESP_LOGE(TAG, "Sensor %d is not enabled", sensor_idx);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "One-shot read: sensor %d (%s), timeout %lu ms",
             sensor_idx, INKBIRD_SENSORS[sensor_idx].name, timeout_ms);

    // Close any existing connection first
    if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "Closing existing connection before new read");
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for disconnect to complete
    }

    // Drain any pending semaphore signals from previous operations
    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {
        // Consume stale signals
    }

    // Set up for this sensor
    s_current_sensor_index = sensor_idx;
    s_data_received = false;
    s_connected = false;  // Reset connection state
    memcpy(s_target_bda, INKBIRD_SENSORS[sensor_idx].mac, 6);

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             s_target_bda[0], s_target_bda[1], s_target_bda[2],
             s_target_bda[3], s_target_bda[4], s_target_bda[5]);

    // Open connection (try public address first)
    esp_err_t ret = esp_ble_gattc_open(
        s_gattc_if, s_target_bda,
        BLE_ADDR_TYPE_PUBLIC, true);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GATTC open failed with public addr: %s, trying random",
                 esp_err_to_name(ret));
        ret = esp_ble_gattc_open(
            s_gattc_if, s_target_bda,
            BLE_ADDR_TYPE_RANDOM, true);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initiate connection: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for data or timeout
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem,
                                         pdMS_TO_TICKS(timeout_ms));

    esp_err_t result;
    if (got_sem == pdTRUE && s_data_received) {
        ESP_LOGI(TAG, "One-shot read successful for sensor %d", sensor_idx);

        // Copy reading if output pointer provided
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

    // Disconnect if still connected
    if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for disconnect to complete
    }
    s_connected = false;  // Ensure state is reset

    return result;
}

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

    // Wait for scan to complete (scan is started in GAP callback after params are set)
    s_scanning = true;
    vTaskDelay(pdMS_TO_TICKS((INKBIRD_SCAN_DURATION_SEC + 2) * 1000));
    s_scanning = false;

    // Stop scan
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

const char *inkbird_ble_get_sensor_name(uint8_t index)
{
    if (index >= INKBIRD_SENSOR_COUNT) {
        return "Unknown";
    }
    return INKBIRD_SENSORS[index].name;
}

bool inkbird_ble_is_sensor_enabled(uint8_t index)
{
    if (index >= INKBIRD_SENSOR_COUNT) {
        return false;
    }
    return INKBIRD_SENSORS[index].enabled;
}

// ============================================================================
// GAP Event Handler
// ============================================================================

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan params set, starting scan...");
            esp_ble_gap_start_scanning(INKBIRD_SCAN_DURATION_SEC);
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Scan start failed: %d", param->scan_start_cmpl.status);
            } else {
                ESP_LOGI(TAG, "Scan started");
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                // Check device name for Inkbird pattern
                uint8_t *adv_name = NULL;
                uint8_t adv_name_len = 0;
                adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                     ESP_BLE_AD_TYPE_NAME_CMPL,
                                                     &adv_name_len);
                if (adv_name == NULL) {
                    adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                         ESP_BLE_AD_TYPE_NAME_SHORT,
                                                         &adv_name_len);
                }

                char name[32] = "(no name)";
                if (adv_name != NULL && adv_name_len > 0) {
                    size_t len = adv_name_len > 31 ? 31 : adv_name_len;
                    memcpy(name, adv_name, len);
                    name[len] = '\0';
                }

                // Check if Inkbird device (by name pattern)
                bool is_inkbird = false;
                if (adv_name != NULL) {
                    if (strncasecmp(name, "Inkbird", 7) == 0 ||
                        strncasecmp(name, "Ink@", 4) == 0 ||
                        strncasecmp(name, "sps", 3) == 0 ||
                        strncasecmp(name, "IAM-T1", 6) == 0 ||
                        strncasecmp(name, "TH", 2) == 0) {
                        is_inkbird = true;
                    }
                }

                // Also check for service UUID in advertisement
                uint8_t *srv_uuid = NULL;
                uint8_t srv_uuid_len = 0;
                srv_uuid = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                     ESP_BLE_AD_TYPE_16SRV_CMPL,
                                                     &srv_uuid_len);
                if (srv_uuid != NULL && srv_uuid_len >= 2) {
                    uint16_t uuid16 = srv_uuid[0] | (srv_uuid[1] << 8);
                    if (uuid16 == INKBIRD_SERVICE_UUID) {
                        is_inkbird = true;
                    }
                }

                if (is_inkbird && s_discovered_count < INKBIRD_MAX_DISCOVERED) {
                    // Check if already discovered
                    bool already_found = false;
                    for (int i = 0; i < s_discovered_count; i++) {
                        if (memcmp(s_discovered[i].mac, param->scan_rst.bda, 6) == 0) {
                            already_found = true;
                            break;
                        }
                    }

                    if (!already_found) {
                        inkbird_discovered_t *d = &s_discovered[s_discovered_count];
                        memcpy(d->mac, param->scan_rst.bda, 6);
                        d->rssi = param->scan_rst.rssi;
                        d->addr_type = param->scan_rst.ble_addr_type;
                        strncpy(d->name, name, sizeof(d->name) - 1);

                        ESP_LOGI(TAG, "Found Inkbird: %02X:%02X:%02X:%02X:%02X:%02X RSSI:%d Name:%s",
                                 d->mac[0], d->mac[1], d->mac[2],
                                 d->mac[3], d->mac[4], d->mac[5],
                                 d->rssi, d->name);

                        s_discovered_count++;
                    }
                }
            } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                ESP_LOGI(TAG, "Scan complete");
                s_scanning = false;
            }
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan stopped");
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGD(TAG, "Connection params updated");
            break;

        default:
            ESP_LOGD(TAG, "GAP event: %d", event);
            break;
    }
}

// ============================================================================
// GATTC Event Handler
// ============================================================================

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = param;

    switch (event) {
        case ESP_GATTC_REG_EVT:
            if (param->reg.status == ESP_GATT_OK) {
                s_gattc_if = gattc_if;
                ESP_LOGI(TAG, "GATTC registered, app_id: %d, if: %d", param->reg.app_id, gattc_if);
            } else {
                ESP_LOGE(TAG, "GATTC register failed: %d", param->reg.status);
            }
            break;

        case ESP_GATTC_CONNECT_EVT:
            ESP_LOGI(TAG, "Connected, conn_id: %d", p_data->connect.conn_id);
            break;

        case ESP_GATTC_OPEN_EVT:
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Open failed, status: %d", param->open.status);
                s_connected = false;
                xSemaphoreGive(s_read_complete_sem);
            } else {
                ESP_LOGI(TAG, "Open success, conn_id: %d", param->open.conn_id);
                s_conn_id = param->open.conn_id;
                s_connected = true;

                // Reset handles
                s_service_start_handle = 0;
                s_service_end_handle = 0;
                s_data_char_handle = 0;
                s_cmd_char_handle = 0;
                s_cccd_handle = 0;

                // Request MTU
                esp_err_t ret = esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "MTU request failed: %s", esp_err_to_name(ret));
                    // Continue anyway - discover services
                    esp_ble_gattc_search_service(gattc_if, param->open.conn_id, NULL);
                }
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            if (param->cfg_mtu.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "MTU config failed: %d", param->cfg_mtu.status);
            } else {
                ESP_LOGI(TAG, "MTU configured: %d", param->cfg_mtu.mtu);
            }
            // Discover services
            esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, NULL);
            break;

        case ESP_GATTC_SEARCH_RES_EVT: {
            ESP_LOGI(TAG, "Service found: UUID 0x%04X, start: %d, end: %d",
                     p_data->search_res.srvc_id.uuid.uuid.uuid16,
                     p_data->search_res.start_handle,
                     p_data->search_res.end_handle);

            if (p_data->search_res.srvc_id.uuid.uuid.uuid16 == INKBIRD_SVC_UUID16) {
                ESP_LOGI(TAG, ">>> Found Inkbird FFE0 service!");
                s_service_start_handle = p_data->search_res.start_handle;
                s_service_end_handle = p_data->search_res.end_handle;
            }
            break;
        }

        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (param->search_cmpl.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Service search failed: %d", param->search_cmpl.status);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            ESP_LOGI(TAG, "Service discovery complete");

            if (s_service_start_handle == 0) {
                ESP_LOGW(TAG, "Inkbird service not found!");
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            // Get all characteristics in the Inkbird service
            uint16_t count = 0;
            esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                gattc_if, s_conn_id, ESP_GATT_DB_CHARACTERISTIC,
                s_service_start_handle, s_service_end_handle,
                INVALID_HANDLE, &count);

            if (status != ESP_GATT_OK || count == 0) {
                ESP_LOGW(TAG, "No characteristics found");
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            ESP_LOGI(TAG, "Found %d characteristics", count);

            esp_gattc_char_elem_t *char_elem = malloc(sizeof(esp_gattc_char_elem_t) * count);
            if (char_elem == NULL) {
                ESP_LOGE(TAG, "malloc failed");
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            status = esp_ble_gattc_get_all_char(
                gattc_if, s_conn_id,
                s_service_start_handle, s_service_end_handle,
                char_elem, &count, 0);

            if (status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Get all chars failed: %d", status);
                free(char_elem);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            for (int i = 0; i < count; i++) {
                uint16_t uuid16 = char_elem[i].uuid.uuid.uuid16;
                ESP_LOGI(TAG, "Char %d: UUID=0x%04X handle=%d props=0x%02X",
                         i, uuid16, char_elem[i].char_handle, char_elem[i].properties);

                if (uuid16 == INKBIRD_DATA_UUID16) {
                    s_data_char_handle = char_elem[i].char_handle;
                    ESP_LOGI(TAG, ">>> Data char FFE4: handle=%d", s_data_char_handle);
                } else if (uuid16 == INKBIRD_CMD_UUID16) {
                    s_cmd_char_handle = char_elem[i].char_handle;
                    ESP_LOGI(TAG, ">>> Cmd char FFE9: handle=%d", s_cmd_char_handle);
                }
            }
            free(char_elem);

            if (s_data_char_handle == 0) {
                ESP_LOGW(TAG, "Data characteristic FFE4 not found!");
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            // Get descriptors for data characteristic
            count = 0;
            status = esp_ble_gattc_get_attr_count(
                gattc_if, s_conn_id, ESP_GATT_DB_DESCRIPTOR,
                s_service_start_handle, s_service_end_handle,
                s_data_char_handle, &count);

            ESP_LOGI(TAG, "Found %d descriptors for FFE4", count);

            if (count > 0) {
                esp_gattc_descr_elem_t *descr_elem = malloc(sizeof(esp_gattc_descr_elem_t) * count);
                if (descr_elem != NULL) {
                    status = esp_ble_gattc_get_all_descr(
                        gattc_if, s_conn_id,
                        s_data_char_handle,
                        descr_elem, &count, 0);

                    if (status == ESP_GATT_OK) {
                        for (int i = 0; i < count; i++) {
                            ESP_LOGI(TAG, "Descr %d: UUID=0x%04X handle=%d",
                                     i, descr_elem[i].uuid.uuid.uuid16, descr_elem[i].handle);

                            if (descr_elem[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                                s_cccd_handle = descr_elem[i].handle;
                                ESP_LOGI(TAG, ">>> CCCD handle: %d", s_cccd_handle);
                            }
                        }
                    }
                    free(descr_elem);
                }
            }

            // Register for notifications
            if (s_data_char_handle != 0) {
                ESP_LOGI(TAG, "Registering for notifications on FFE4...");
                esp_err_t ret = esp_ble_gattc_register_for_notify(
                    gattc_if, s_target_bda, s_data_char_handle);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Register for notify failed: %s", esp_err_to_name(ret));
                    xSemaphoreGive(s_read_complete_sem);
                }
            }
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Register for notify failed: %d", param->reg_for_notify.status);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            ESP_LOGI(TAG, "Registered for notify, handle: %d", param->reg_for_notify.handle);

            // Write CCCD to enable notifications (0x0001)
            if (s_cccd_handle != 0) {
                uint16_t notify_enable = 0x0001;
                esp_err_t ret = esp_ble_gattc_write_char_descr(
                    gattc_if, s_conn_id, s_cccd_handle,
                    sizeof(notify_enable), (uint8_t *)&notify_enable,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Write CCCD failed: %s", esp_err_to_name(ret));
                    xSemaphoreGive(s_read_complete_sem);
                } else {
                    ESP_LOGI(TAG, "CCCD write initiated (enable notifications)");
                }
            } else {
                // No CCCD found, try writing to handle+1
                ESP_LOGW(TAG, "No CCCD found, trying handle+1");
                uint16_t notify_enable = 0x0001;
                esp_ble_gattc_write_char_descr(
                    gattc_if, s_conn_id, s_data_char_handle + 1,
                    sizeof(notify_enable), (uint8_t *)&notify_enable,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            }
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Write descriptor failed: %d", param->write.status);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }
            ESP_LOGI(TAG, "CCCD write success - notifications enabled!");

            // For history download mode, signal that setup is complete so we can
            // proceed to send the history command.
            // For normal mode, we wait for the first notification to arrive.
            if (s_history_state != INKBIRD_HISTORY_IDLE) {
                ESP_LOGI(TAG, "History mode: signaling setup complete");
                xSemaphoreGive(s_read_complete_sem);
            } else {
                ESP_LOGI(TAG, "Waiting for sensor data (may take a few seconds)...");
            }
            break;

        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.value_len > 0) {
                // Check if we're in history download mode
                if (s_history_state == INKBIRD_HISTORY_REQUESTING ||
                    s_history_state == INKBIRD_HISTORY_RECEIVING) {
                    // History mode - minimal logging to avoid stack overflow
                    parse_history_notification(param->notify.value, param->notify.value_len);
                } else {
                    // Normal mode - log details
                    ESP_LOGI(TAG, "Notification: handle=%d, len=%d", param->notify.handle, param->notify.value_len);
                    ESP_LOG_BUFFER_HEX(TAG, param->notify.value, param->notify.value_len);
                    // Normal real-time data
                    // Copy data
                    s_recv_len = param->notify.value_len;
                    if (s_recv_len > sizeof(s_recv_data)) {
                        s_recv_len = sizeof(s_recv_data);
                    }
                    memcpy(s_recv_data, param->notify.value, s_recv_len);
                    s_data_received = true;

                    // Parse data
                    parse_inkbird_data(s_recv_data, s_recv_len, s_current_sensor_index);

                    // Signal completion
                    xSemaphoreGive(s_read_complete_sem);
                }
            }
            break;

        case ESP_GATTC_CLOSE_EVT:
        case ESP_GATTC_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Disconnected");
            s_connected = false;
            xSemaphoreGive(s_read_complete_sem);
            break;

        default:
            ESP_LOGD(TAG, "GATTC event: %d", event);
            break;
    }
}

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
 */
static void parse_inkbird_data(const uint8_t *data, size_t len, uint8_t sensor_idx)
{
    if (sensor_idx >= INKBIRD_SENSOR_COUNT) {
        return;
    }

    ESP_LOGI(TAG, "=== PARSING DATA ===");
    ESP_LOGI(TAG, "Length: %d bytes", len);
    if (len > 0) {
        ESP_LOG_BUFFER_HEX(TAG, data, len > 20 ? 20 : len);
    }

    // ESPHome format: starts with 0x55
    // Check: x[0] != 0x55 && (x[4] & 0xf0) != 0 => return NAN
    if (len >= 13 && data[0] == 0x55 && (data[4] & 0xF0) == 0) {
        ESP_LOGI(TAG, "Parsing ESPHome format (0x55 header)");

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
        ESP_LOGI(TAG, "  Sensor %d [%s]:", sensor_idx, INKBIRD_SENSORS[sensor_idx].name);
        ESP_LOGI(TAG, "  CO2: %u ppm", reading->co2_ppm);
        ESP_LOGI(TAG, "  Temperature: %.1f C", reading->temperature / 10.0f);
        ESP_LOGI(TAG, "  Humidity: %.1f%%", reading->humidity / 10.0f);
        ESP_LOGI(TAG, "  Pressure: %u hPa", reading->pressure);
        return;
    }

    // Also try AA 01 format (from Python library)
    if (len >= 16 && data[0] == 0xAA && data[1] == 0x01) {
        ESP_LOGI(TAG, "Parsing Python library format (0xAA 0x01 header)");

        xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
        inkbird_reading_t *reading = &s_readings[sensor_idx];

        // Temperature (bytes 4-6)
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
        ESP_LOGI(TAG, "  Sensor %d [%s]:", sensor_idx, INKBIRD_SENSORS[sensor_idx].name);
        ESP_LOGI(TAG, "  CO2: %u ppm", reading->co2_ppm);
        ESP_LOGI(TAG, "  Temperature: %.1f C", reading->temperature / 10.0f);
        ESP_LOGI(TAG, "  Humidity: %.1f%%", reading->humidity / 10.0f);
        ESP_LOGI(TAG, "  Pressure: %u hPa", reading->pressure);
        return;
    }

    ESP_LOGW(TAG, "Unknown data format: len=%d", len);
    if (len >= 4) {
        ESP_LOGW(TAG, "  First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X",
                 data[0], data[1], data[2], data[3]);
    }
}

// ============================================================================
// Read Task
// ============================================================================

static void read_task(void *arg)
{
    ESP_LOGI(TAG, "Read task started");

    while (s_running) {
        // Round-robin through all sensors
        for (int i = 0; i < INKBIRD_SENSOR_COUNT && s_running; i++) {
            // Skip if sensor not enabled
            if (!INKBIRD_SENSORS[i].enabled) {
                continue;
            }

            // Skip if in skip period after failures
            if (s_skip_cycles[i] > 0) {
                s_skip_cycles[i]--;
                ESP_LOGD(TAG, "Skipping sensor %d, %d cycles remaining", i, s_skip_cycles[i]);
                continue;
            }

            ESP_LOGI(TAG, "Reading sensor %d: %s", i, INKBIRD_SENSORS[i].name);
            s_current_sensor_index = i;
            s_data_received = false;

            // Copy target MAC address
            memcpy(s_target_bda, INKBIRD_SENSORS[i].mac, 6);

            ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
                     s_target_bda[0], s_target_bda[1], s_target_bda[2],
                     s_target_bda[3], s_target_bda[4], s_target_bda[5]);

            // Open connection
            esp_err_t ret = esp_ble_gattc_open(
                s_gattc_if, s_target_bda,
                BLE_ADDR_TYPE_PUBLIC, true);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "GATTC open failed: %s, trying random addr", esp_err_to_name(ret));
                ret = esp_ble_gattc_open(
                    s_gattc_if, s_target_bda,
                    BLE_ADDR_TYPE_RANDOM, true);
            }

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to connect to sensor %d: %s", i, esp_err_to_name(ret));
                s_failure_count[i]++;

                if (s_failure_count[i] >= INKBIRD_MAX_FAILURES) {
                    ESP_LOGW(TAG, "Sensor %d: %d failures, skipping %d cycles",
                             i, s_failure_count[i], INKBIRD_SKIP_CYCLES_ON_FAILURE);
                    s_skip_cycles[i] = INKBIRD_SKIP_CYCLES_ON_FAILURE;
                    s_failure_count[i] = 0;
                }
                continue;
            }

            // Wait for data or timeout
            BaseType_t got_data = xSemaphoreTake(s_read_complete_sem,
                                                  pdMS_TO_TICKS(INKBIRD_CONNECT_TIMEOUT_MS));

            if (got_data == pdTRUE && s_data_received) {
                ESP_LOGI(TAG, "Successfully read sensor %d", i);
                s_failure_count[i] = 0;
            } else {
                ESP_LOGW(TAG, "Timeout or no data from sensor %d", i);
                s_failure_count[i]++;

                if (s_failure_count[i] >= INKBIRD_MAX_FAILURES) {
                    ESP_LOGW(TAG, "Sensor %d: %d failures, skipping %d cycles",
                             i, s_failure_count[i], INKBIRD_SKIP_CYCLES_ON_FAILURE);
                    s_skip_cycles[i] = INKBIRD_SKIP_CYCLES_ON_FAILURE;
                    s_failure_count[i] = 0;
                }
            }

            // Disconnect if still connected
            if (s_connected && s_gattc_if != ESP_GATT_IF_NONE) {
                esp_ble_gattc_close(s_gattc_if, s_conn_id);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            // Delay between sensors
            vTaskDelay(pdMS_TO_TICKS(INKBIRD_INTER_SENSOR_DELAY_MS));
        }

        // Wait until next read cycle
        if (s_running) {
            ESP_LOGI(TAG, "Read cycle complete, next in %d seconds",
                     INKBIRD_READ_INTERVAL_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(INKBIRD_READ_INTERVAL_MS));
        }
    }

    ESP_LOGI(TAG, "Read task exiting");
    vTaskDelete(NULL);
}

// ============================================================================
// History Download Implementation
// ============================================================================

/**
 * @brief Send a command to the sensor via FFE9
 */
static esp_err_t send_history_command(const uint8_t *cmd, size_t len)
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
static bool is_empty_record(const uint8_t *data)
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
 * - Byte 2 upper nibble or Byte 3 lower: Temperature sign (0=positive, 1=negative)
 * - Bytes 3-4: Temperature * 10 (big-endian)
 * - Bytes 5-6: Humidity * 10 (big-endian)
 * - Bytes 7-8: Pressure/HAP (big-endian)
 * - Byte 9: Time interval in minutes
 *
 * Uses circular buffer to keep the NEWEST records:
 * - Sensor sends oldest->newest
 * - We write to circular buffer, overwriting oldest as we go
 * - When complete, buffer contains the newest N records
 *
 * Returns true if record was stored, false if skipped (empty)
 */
static bool parse_history_record(const uint8_t *data)
{
    // Skip empty records (all 0xFF = uninitialized flash memory)
    if (is_empty_record(data)) {
        return false;  // Skipped
    }

    if (s_history_records == NULL || s_history_max_records == 0) {
        return false;
    }

    // Parse CO2 first to validate
    uint16_t co2_ppm = ((uint16_t)data[0] << 8) | data[1];
    
    // Skip records with invalid CO2 values (sanity check)
    // Valid range: 200-10000 ppm (outdoor is ~400, very poor indoor can reach 5000+)
    if (co2_ppm < 200 || co2_ppm > 10000) {
        return false;  // Invalid record, skip
    }

    // Write to current position in circular buffer
    inkbird_history_record_t *rec = &s_history_records[s_history_write_idx];

    // CO2: bytes 0-1 (big-endian)
    rec->co2_ppm = co2_ppm;

    // Byte 2 structure (from INKBIRD_IAM_T1_PROTOCOL.md section 6.3):
    // In hex string: position [4] = TempUnit, position [5] = TempSign
    // In raw bytes: upper nibble = TempUnit, lower nibble = TempSign
    rec->is_fahrenheit = (data[2] & 0xF0) != 0;  // Upper nibble = TempUnit
    bool is_negative = (data[2] & 0x0F) != 0;    // Lower nibble = TempSign

    // Temperature: bytes 3-4 (big-endian) * 0.1
    uint16_t temp_raw = ((uint16_t)data[3] << 8) | data[4];
    rec->temperature = is_negative ? -(int16_t)temp_raw : (int16_t)temp_raw;

    // Humidity: bytes 5-6 (big-endian) * 0.1
    rec->humidity = ((uint16_t)data[5] << 8) | data[6];

    // Pressure: bytes 7-8 (big-endian)
    rec->pressure = ((uint16_t)data[7] << 8) | data[8];

    // Interval: byte 9
    rec->interval_mins = data[9];

    s_history_received_count++;  // Total valid records seen

    ESP_LOGD(TAG, "History[%d->%d]: CO2=%u, T=%d, H=%u, P=%u, Int=%u",
             s_history_received_count, s_history_write_idx, rec->co2_ppm,
             rec->temperature, rec->humidity, rec->pressure, rec->interval_mins);

    // Advance write index (circular)
    s_history_write_idx++;
    if (s_history_write_idx >= s_history_max_records) {
        s_history_write_idx = 0;
        s_history_buffer_wrapped = true;
    }

    return true;  // Record stored
}

/**
 * @brief Parse history notification data
 *
 * Protocol (per INKBIRD_IAM_T1_PROTOCOL.md section 6):
 * 1. First packet: 2 bytes = record count (big-endian)
 * 2. Data packets: 10 bytes per record (raw bytes, fragmented across BLE packets)
 * 3. End marker: 0x66 0x66
 */
static void parse_history_notification(const uint8_t *data, size_t len)
{
    // Debug logging for history packets (use ESP_LOGD in production)
    ESP_LOGD(TAG, "History RX: %d bytes, first 4: %02X %02X %02X %02X", 
             (int)len, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);

    // Check for end marker (0x6666) anywhere in packet
    // End marker can be at start OR after partial record data
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == HISTORY_END_MARKER_HIGH && data[i + 1] == HISTORY_END_MARKER_LOW) {
            ESP_LOGI(TAG, "History end marker in packet at offset %d", (int)i);
            // Add data before the end marker to buffer for processing
            if (i > 0 && s_history_buffer_len + i < sizeof(s_history_buffer)) {
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
    // Per INKBIRD_IAM_T1_PROTOCOL.md section 6.2: "4 hex chars (2 bytes)"
    if (s_history_state == INKBIRD_HISTORY_REQUESTING && !s_history_got_count) {
        if (len >= 2) {
            // Count is 2 bytes big-endian
            s_history_expected_count = ((uint16_t)data[0] << 8) | data[1];
            s_history_got_count = true;
            s_history_state = INKBIRD_HISTORY_RECEIVING;
            ESP_LOGI(TAG, ">>> History record count: %u (raw: 0x%02X%02X) <<<", 
                     s_history_expected_count, data[0], data[1]);

            // Process any remaining data in this packet
            if (len > 2) {
                size_t remaining = len - 2;
                if (remaining + s_history_buffer_len < sizeof(s_history_buffer)) {
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
        if (len + s_history_buffer_len < sizeof(s_history_buffer)) {
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

            // Parse one record (skips empty records, uses circular buffer)
            parse_history_record(s_history_buffer);

            // Shift buffer
            memmove(s_history_buffer, s_history_buffer + 10, s_history_buffer_len - 10);
            s_history_buffer_len -= 10;

            // Log progress periodically (every 5000 valid records)
            if (s_history_received_count > 0 && s_history_received_count % 5000 == 0) {
                ESP_LOGI(TAG, "Progress: %u valid records received...", s_history_received_count);
            }
        }

        // Check for end marker in remaining buffer (less than 10 bytes)
        // End marker can appear after last record data: e.g., "E3 66 66" where E3 is 
        // last byte of record and 66 66 is end marker
        if (s_history_buffer_len >= 2) {
            // Scan buffer for end marker anywhere
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

    if (sensor_idx >= INKBIRD_SENSOR_COUNT || !INKBIRD_SENSORS[sensor_idx].enabled) {
        ESP_LOGE(TAG, "Invalid or disabled sensor index: %d", sensor_idx);
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

    // Reset history state - set to REQUESTING before connect so CCCD handler knows
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
    ESP_LOGI(TAG, "  Sensor: %d (%s)", sensor_idx, INKBIRD_SENSORS[sensor_idx].name);
    ESP_LOGI(TAG, "  Max records: %u", max_records);
    ESP_LOGI(TAG, "========================================");

    // Connect to sensor
    s_current_sensor_index = sensor_idx;
    memcpy(s_target_bda, INKBIRD_SENSORS[sensor_idx].mac, 6);
    s_data_received = false;

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X...",
             s_target_bda[0], s_target_bda[1], s_target_bda[2],
             s_target_bda[3], s_target_bda[4], s_target_bda[5]);

    // Drain any pending semaphore signals from previous operations
    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {
        // Consume any stale signals
    }

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
        s_history_state = INKBIRD_HISTORY_ERROR;
        return ret;
    }

    // Wait for connection and notification setup
    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem, pdMS_TO_TICKS(30000));
    if (got_sem != pdTRUE || !s_connected) {
        ESP_LOGE(TAG, "Connection timeout or failed");
        s_history_state = INKBIRD_HISTORY_ERROR;
        if (s_connected) {
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
        }
        return ESP_ERR_TIMEOUT;
    }

    // Small delay after notification setup
    vTaskDelay(pdMS_TO_TICKS(500));

    // Send history start command
    ESP_LOGI(TAG, "Sending history start command...");

    ret = send_history_command(CMD_HISTORY_START, sizeof(CMD_HISTORY_START));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send history command");
        s_history_state = INKBIRD_HISTORY_ERROR;
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        return ret;
    }

    // Wait for history download to complete (timeout: 5 minutes for large datasets)
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

    // Calculate actual stored count and reorder circular buffer
    // The circular buffer now contains the NEWEST records, but they may be
    // out of order if the buffer wrapped.
    if (s_history_buffer_wrapped) {
        // Buffer wrapped - records are out of order
        // Current layout: [newest...] [oldest in buffer...]
        //                  ^write_idx
        // We need to reorder to: [oldest in buffer...] [newest...]
        s_history_stored_count = s_history_max_records;

        ESP_LOGI(TAG, "Reordering circular buffer (wrapped at idx %u)...", s_history_write_idx);

        // Allocate temp buffer for reordering
        inkbird_history_record_t *temp = pvPortMalloc(sizeof(inkbird_history_record_t) * s_history_max_records);
        if (temp != NULL) {
            // Copy from write_idx to end (these are the older records in buffer)
            uint16_t first_part = s_history_max_records - s_history_write_idx;
            memcpy(temp, &records[s_history_write_idx], sizeof(inkbird_history_record_t) * first_part);

            // Copy from start to write_idx (these are the newer records)
            memcpy(&temp[first_part], records, sizeof(inkbird_history_record_t) * s_history_write_idx);

            // Copy back to original buffer
            memcpy(records, temp, sizeof(inkbird_history_record_t) * s_history_max_records);

            vPortFree(temp);
            ESP_LOGI(TAG, "Buffer reordered: oldest at [0], newest at [%u]", s_history_max_records - 1);
        } else {
            ESP_LOGW(TAG, "Failed to allocate temp buffer for reordering");
            // Records will be out of order but still valid
        }
    } else {
        // Buffer didn't wrap - records are already in order (oldest to newest)
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

    // Determine success and reset state
    bool success = (s_history_state == INKBIRD_HISTORY_COMPLETE ||
        (s_history_stored_count > 0 && s_history_received_count > s_history_expected_count * 9 / 10));
    
    // Reset state to IDLE so normal BLE reading works
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

    esp_err_t ret = send_history_command(CMD_HISTORY_STOP, sizeof(CMD_HISTORY_STOP));

    s_history_state = INKBIRD_HISTORY_IDLE;
    s_history_records = NULL;
    s_history_buffer_len = 0;

    return ret;
}

inkbird_history_state_t inkbird_ble_get_history_state(void)
{
    return s_history_state;
}

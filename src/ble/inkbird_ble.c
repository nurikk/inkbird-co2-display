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
// Forward Declarations
// ============================================================================

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void read_task(void *arg);
static void parse_inkbird_data(const uint8_t *data, size_t len, uint8_t sensor_idx);

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
            ESP_LOGI(TAG, "Waiting for sensor data (may take a few seconds)...");
            break;

        case ESP_GATTC_NOTIFY_EVT:
            ESP_LOGI(TAG, "=== NOTIFICATION RECEIVED ===");
            ESP_LOGI(TAG, "  handle: %d, len: %d", param->notify.handle, param->notify.value_len);
            if (param->notify.value_len > 0) {
                ESP_LOG_BUFFER_HEX(TAG, param->notify.value, param->notify.value_len);

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

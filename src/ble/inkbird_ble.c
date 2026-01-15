/**
 * @file inkbird_ble.c
 * @brief BLE reader implementation for Inkbird IAM-T1 CO2 sensors (NimBLE)
 *
 * Uses NimBLE stack to scan for, connect to, and read data from
 * Inkbird IAM-T1 sensors via GATT notifications.
 */

#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "inkbird_ble.h"
#include "inkbird_config.h"
#include "sensor_data.h"

void ble_store_config_init(void);

static const char *TAG = "inkbird_ble";

#define BLE_TASK_CORE           0
#define INKBIRD_SVC_UUID16      0xFFE0
#define INKBIRD_DATA_UUID16     0xFFE4
#define INKBIRD_CMD_UUID16      0xFFE9
#define CCCD_UUID16             0x2902

static const uint8_t CMD_REALTIME_DATA[] = {0x55, 0xAA, 0x09, 0x06, 0x01, 0x0F};
static const uint8_t CMD_PAIRING[] = {0x55, 0xAA, 0x08, 0x06, 0x01, 0x0E};
static const uint8_t CMD_CO2_SETTINGS[]  = {0x55, 0xAA, 0x02, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C};
static const uint8_t CMD_CO2_THRESHOLDS[] = {0x55, 0xAA, 0x03, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
static const uint8_t CMD_HISTORY_START[] = {0x55, 0xAA, 0x07, 0x06, 0x00, 0x0C};
static const uint8_t CMD_HISTORY_STOP[]  = {0x55, 0xAA, 0x07, 0x06, 0x01, 0x0D};

#define HISTORY_END_MARKER_HIGH 0x66
#define HISTORY_END_MARKER_LOW  0x66
#define HISTORY_EMPTY_BYTE 0xFF

static inkbird_reading_t s_readings[INKBIRD_SENSOR_COUNT];
static inkbird_co2_thresholds_t s_thresholds[INKBIRD_SENSOR_COUNT];
static uint8_t s_failure_count[INKBIRD_SENSOR_COUNT];
static uint8_t s_skip_cycles[INKBIRD_SENSOR_COUNT];

static inkbird_sensor_config_t s_active_sensors[INKBIRD_SENSOR_COUNT];
static uint8_t s_active_sensor_count = 0;

static inkbird_discovered_t s_discovered[INKBIRD_MAX_DISCOVERED];
static uint8_t s_discovered_count = 0;

static bool s_ble_initialized = false;
static bool s_running = false;
static bool s_scanning = false;
static bool s_connected = false;
static uint16_t s_conn_handle = 0;
static uint8_t s_current_sensor_index = 0;

static uint16_t s_service_start_handle = 0;
static uint16_t s_service_end_handle = 0;
static uint16_t s_data_char_handle = 0;
static uint16_t s_cmd_char_handle = 0;
static uint16_t s_cccd_handle = 0;

static SemaphoreHandle_t s_ble_mutex = NULL;
static SemaphoreHandle_t s_read_complete_sem = NULL;

static TaskHandle_t s_read_task_handle = NULL;

static uint8_t s_recv_data[32];
static size_t s_recv_len = 0;
static bool s_data_received = false;

static ble_addr_t s_target_addr;

static inkbird_history_state_t s_history_state = INKBIRD_HISTORY_IDLE;
static inkbird_history_record_t *s_history_records = NULL;
static uint16_t s_history_max_records = 0;
static uint16_t s_history_expected_count = 0;
static uint16_t s_history_received_count = 0;
static uint16_t s_history_stored_count = 0;
static bool s_history_got_count = false;
static uint8_t s_history_buffer[256];
static size_t s_history_buffer_len = 0;
static SemaphoreHandle_t s_history_complete_sem = NULL;
static uint16_t s_history_write_idx = 0;
static bool s_history_buffer_wrapped = false;

static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void read_task(void *arg);
static bool parse_inkbird_data(const uint8_t *data, size_t len, uint8_t sensor_idx);
static void parse_history_notification(const uint8_t *data, size_t len);
static bool is_empty_record(const uint8_t *data);
static bool parse_history_record(const uint8_t *data);
static void on_sync(void);
static void on_reset(int reason);
static void nimble_host_task(void *param);

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t inkbird_ble_init(void)
{
    if (s_ble_initialized) {
        ESP_LOGW(TAG, "BLE already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing BLE (NimBLE stack) for Inkbird sensors...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    memset(s_readings, 0, sizeof(s_readings));
    memset(s_thresholds, 0, sizeof(s_thresholds));
    memset(s_failure_count, 0, sizeof(s_failure_count));
    memset(s_skip_cycles, 0, sizeof(s_skip_cycles));

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

    for (int i = 0; i < INKBIRD_SENSOR_COUNT; i++) {
        s_thresholds[i].normal_low_ppm = 420;
        s_thresholds[i].normal_high_ppm = 2000;
        s_thresholds[i].plant_low_ppm = 340;
        s_thresholds[i].plant_high_ppm = 5000;
        s_thresholds[i].use_custom = false;
        s_thresholds[i].settings_valid = false;
        s_thresholds[i].thresholds_valid = true;
    }

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

    ESP_LOGI(TAG, "Free heap before NimBLE: %lu bytes", esp_get_free_heap_size());

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE port: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);

    vTaskDelay(pdMS_TO_TICKS(500));

    s_ble_initialized = true;
    ESP_LOGI(TAG, "BLE initialized successfully (NimBLE stack)");
    ESP_LOGI(TAG, "Free heap after NimBLE: %lu bytes", esp_get_free_heap_size());

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

    if (s_active_sensor_count == 0) {
        ESP_LOGW(TAG, "No active sensors");
    }

    s_running = true;

    BaseType_t xret = xTaskCreatePinnedToCore(
        read_task,
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

    if (s_read_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_read_task_handle = NULL;
    }

    if (s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
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

static int service_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg);
static int char_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg);
static int desc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);
static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg);

static int service_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service != NULL) {
        if (ble_uuid_u16(&service->uuid.u) == INKBIRD_SVC_UUID16) {
            ESP_LOGI(TAG, "Found Inkbird service FFE0: start=%d end=%d",
                     service->start_handle, service->end_handle);
            s_service_start_handle = service->start_handle;
            s_service_end_handle = service->end_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Service discovery complete");
        if (s_service_start_handle != 0) {
            ble_gattc_disc_all_chrs(conn_handle, s_service_start_handle,
                                     s_service_end_handle, char_disc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "Inkbird service not found");
            xSemaphoreGive(s_read_complete_sem);
        }
    }
    return 0;
}

static int char_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        uint16_t uuid16 = ble_uuid_u16(&chr->uuid.u);
        ESP_LOGI(TAG, "Char: UUID=0x%04X handle=%d val_handle=%d props=0x%02X",
                 uuid16, chr->def_handle, chr->val_handle, chr->properties);

        if (uuid16 == INKBIRD_DATA_UUID16) {
            s_data_char_handle = chr->val_handle;
            ESP_LOGI(TAG, ">>> Data char FFE4: handle=%d", s_data_char_handle);
        } else if (uuid16 == INKBIRD_CMD_UUID16) {
            s_cmd_char_handle = chr->val_handle;
            ESP_LOGI(TAG, ">>> Cmd char FFE9: handle=%d", s_cmd_char_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Characteristic discovery complete");
        if (s_data_char_handle != 0) {
            ble_gattc_disc_all_dscs(conn_handle, s_data_char_handle,
                                     s_service_end_handle, desc_disc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "Data characteristic not found");
            xSemaphoreGive(s_read_complete_sem);
        }
    }
    return 0;
}

static int desc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0 && dsc != NULL) {
        uint16_t uuid16 = ble_uuid_u16(&dsc->uuid.u);
        ESP_LOGI(TAG, "Desc: UUID=0x%04X handle=%d", uuid16, dsc->handle);

        if (uuid16 == CCCD_UUID16) {
            s_cccd_handle = dsc->handle;
            ESP_LOGI(TAG, ">>> CCCD handle: %d", s_cccd_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Descriptor discovery complete");

        if (s_cccd_handle != 0) {
            uint8_t value[2] = {0x01, 0x00};
            ESP_LOGI(TAG, "Enabling notifications on CCCD handle %d", s_cccd_handle);
            ble_gattc_write_flat(conn_handle, s_cccd_handle, value, 2, write_cb, (void*)1);
        } else {
            uint8_t value[2] = {0x01, 0x00};
            ESP_LOGW(TAG, "No CCCD found, trying handle+1");
            ble_gattc_write_flat(conn_handle, s_data_char_handle + 1, value, 2, write_cb, (void*)1);
        }
    }
    return 0;
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    int write_type = (int)(intptr_t)arg;

    if (error->status != 0) {
        ESP_LOGW(TAG, "Write failed: status=%d", error->status);
        if (write_type == 1) {
            xSemaphoreGive(s_read_complete_sem);
        }
        return 0;
    }

    if (write_type == 1) {
        ESP_LOGI(TAG, "CCCD write success - notifications enabled");

        if (s_history_state != INKBIRD_HISTORY_IDLE) {
            ESP_LOGI(TAG, "History mode: signaling setup complete");
            xSemaphoreGive(s_read_complete_sem);
        } else {
            sensor_data_set_status(s_current_sensor_index, "Requesting...");
            if (s_cmd_char_handle != 0) {
                ESP_LOGI(TAG, "Sending pairing request...");
                ble_gattc_write_no_rsp_flat(conn_handle, s_cmd_char_handle,
                                             CMD_PAIRING, sizeof(CMD_PAIRING));
                vTaskDelay(pdMS_TO_TICKS(100));

                ESP_LOGI(TAG, "Sending CO2 settings request...");
                ble_gattc_write_no_rsp_flat(conn_handle, s_cmd_char_handle,
                                             CMD_CO2_SETTINGS, sizeof(CMD_CO2_SETTINGS));
                vTaskDelay(pdMS_TO_TICKS(100));

                ESP_LOGI(TAG, "Sending CO2 thresholds request...");
                ble_gattc_write_no_rsp_flat(conn_handle, s_cmd_char_handle,
                                             CMD_CO2_THRESHOLDS, sizeof(CMD_CO2_THRESHOLDS));
                vTaskDelay(pdMS_TO_TICKS(100));

                ESP_LOGI(TAG, "Sending real-time data request...");
                ble_gattc_write_flat(conn_handle, s_cmd_char_handle,
                                      CMD_REALTIME_DATA, sizeof(CMD_REALTIME_DATA),
                                      write_cb, (void*)2);
            }
        }
    } else if (write_type == 2) {
        ESP_LOGI(TAG, "Data request sent, waiting for notification...");
    }
    return 0;
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (s_scanning) {
                struct ble_hs_adv_fields fields;
                ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

                char name[32] = "";
                if (fields.name != NULL && fields.name_len > 0) {
                    size_t len = fields.name_len > 31 ? 31 : fields.name_len;
                    memcpy(name, fields.name, len);
                    name[len] = '\0';
                }

                bool is_inkbird = false;
                if (name[0] != '\0') {
                    if (strncasecmp(name, "Inkbird", 7) == 0 ||
                        strncasecmp(name, "Ink@", 4) == 0 ||
                        strncasecmp(name, "sps", 3) == 0 ||
                        strncasecmp(name, "IAM-T1", 6) == 0 ||
                        strncasecmp(name, "TH", 2) == 0) {
                        is_inkbird = true;
                    }
                }

                if (fields.num_uuids16 > 0) {
                    for (int i = 0; i < fields.num_uuids16; i++) {
                        if (ble_uuid_u16(&fields.uuids16[i].u) == INKBIRD_SERVICE_UUID) {
                            is_inkbird = true;
                            break;
                        }
                    }
                }

                if (is_inkbird && s_discovered_count < INKBIRD_MAX_DISCOVERED) {
                    bool already_found = false;
                    for (int i = 0; i < s_discovered_count; i++) {
                        if (memcmp(s_discovered[i].mac, event->disc.addr.val, 6) == 0) {
                            already_found = true;
                            break;
                        }
                    }

                    if (!already_found) {
                        inkbird_discovered_t *d = &s_discovered[s_discovered_count];
                        memcpy(d->mac, event->disc.addr.val, 6);
                        d->rssi = event->disc.rssi;
                        d->addr_type = event->disc.addr.type;
                        strncpy(d->name, name, sizeof(d->name) - 1);

                        ESP_LOGI(TAG, "Found Inkbird: %02X:%02X:%02X:%02X:%02X:%02X RSSI:%d Name:%s",
                                 d->mac[0], d->mac[1], d->mac[2],
                                 d->mac[3], d->mac[4], d->mac[5],
                                 d->rssi, d->name);

                        s_discovered_count++;
                    }
                }
            }
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "Discovery complete, reason=%d", event->disc_complete.reason);
            s_scanning = false;
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected, conn_handle=%d", event->connect.conn_handle);
                s_conn_handle = event->connect.conn_handle;
                s_connected = true;
                sensor_data_set_status(s_current_sensor_index, "Discovering...");

                s_service_start_handle = 0;
                s_service_end_handle = 0;
                s_data_char_handle = 0;
                s_cmd_char_handle = 0;
                s_cccd_handle = 0;

                ble_gattc_disc_all_svcs(event->connect.conn_handle, service_disc_cb, NULL);
            } else {
                ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
                s_connected = false;
                sensor_data_set_status(s_current_sensor_index, "Connect failed");
                xSemaphoreGive(s_read_complete_sem);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
            s_connected = false;
            xSemaphoreGive(s_read_complete_sem);
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.attr_handle == s_data_char_handle) {
                uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
                uint8_t data[64];
                if (len > sizeof(data)) len = sizeof(data);
                os_mbuf_copydata(event->notify_rx.om, 0, len, data);

                if (s_history_state == INKBIRD_HISTORY_REQUESTING ||
                    s_history_state == INKBIRD_HISTORY_RECEIVING) {
                    parse_history_notification(data, len);
                } else {
                    ESP_LOGI(TAG, "Notification: handle=%d, len=%d", event->notify_rx.attr_handle, len);
                    ESP_LOG_BUFFER_HEX(TAG, data, len);

                    s_recv_len = len;
                    if (s_recv_len > sizeof(s_recv_data)) {
                        s_recv_len = sizeof(s_recv_data);
                    }
                    memcpy(s_recv_data, data, s_recv_len);

                    bool is_realtime = parse_inkbird_data(s_recv_data, s_recv_len, s_current_sensor_index);

                    if (is_realtime) {
                        s_data_received = true;
                        sensor_data_set_status(s_current_sensor_index, NULL);
                        xSemaphoreGive(s_read_complete_sem);
                    }
                }
            }
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU update: conn_handle=%d, mtu=%d",
                     event->mtu.conn_handle, event->mtu.value);
            break;

        default:
            ESP_LOGD(TAG, "GAP event: %d", event->type);
            break;
    }
    return 0;
}

static bool connect_to_sensor(uint8_t sensor_idx)
{
    if (sensor_idx >= s_active_sensor_count) {
        return false;
    }

    s_current_sensor_index = sensor_idx;
    s_data_received = false;

    s_target_addr.type = BLE_ADDR_PUBLIC;
    memcpy(s_target_addr.val, s_active_sensors[sensor_idx].mac, 6);

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             s_target_addr.val[0], s_target_addr.val[1], s_target_addr.val[2],
             s_target_addr.val[3], s_target_addr.val[4], s_target_addr.val[5]);

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_target_addr, 30000,
                              NULL, gap_event_handler, NULL);

    if (rc != 0) {
        ESP_LOGW(TAG, "Connect failed with public addr: %d, trying random", rc);
        s_target_addr.type = BLE_ADDR_RANDOM;
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_target_addr, 30000,
                              NULL, gap_event_handler, NULL);
    }

    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate connection: %d", rc);
        return false;
    }

    return true;
}

esp_err_t inkbird_ble_read_sensor_once(uint8_t sensor_idx, uint32_t timeout_ms,
                                        inkbird_reading_t *out_reading)
{
    if (!s_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sensor_idx >= s_active_sensor_count) {
        ESP_LOGE(TAG, "Invalid sensor index: %d", sensor_idx);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_active_sensors[sensor_idx].enabled) {
        ESP_LOGE(TAG, "Sensor %d is not enabled", sensor_idx);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "One-shot read: sensor %d (%s), timeout %lu ms",
             sensor_idx, s_active_sensors[sensor_idx].name, timeout_ms);

    if (s_connected) {
        ESP_LOGW(TAG, "Closing existing connection");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

    s_current_sensor_index = sensor_idx;
    s_data_received = false;
    s_connected = false;

    if (!connect_to_sensor(sensor_idx)) {
        return ESP_FAIL;
    }

    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem, pdMS_TO_TICKS(timeout_ms));

    esp_err_t result;
    if (got_sem == pdTRUE && s_data_received) {
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

    if (s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_connected = false;

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

    s_discovered_count = 0;
    memset(s_discovered, 0, sizeof(s_discovered));

    struct ble_gap_disc_params disc_params = {
        .itvl = 0x0050,
        .window = 0x0030,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };

    s_scanning = true;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, INKBIRD_SCAN_DURATION_SEC * 1000,
                          &disc_params, gap_event_handler, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
        s_scanning = false;
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS((INKBIRD_SCAN_DURATION_SEC + 2) * 1000));
    s_scanning = false;

    ble_gap_disc_cancel();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Discovery Complete: %d sensor(s) found", s_discovered_count);
    ESP_LOGI(TAG, "========================================");

    if (s_discovered_count == 0) {
        ESP_LOGW(TAG, "No Inkbird sensors found!");
    } else {
        for (int i = 0; i < s_discovered_count; i++) {
            ESP_LOGI(TAG, "[%d] MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
                     i, s_discovered[i].mac[0], s_discovered[i].mac[1],
                     s_discovered[i].mac[2], s_discovered[i].mac[3],
                     s_discovered[i].mac[4], s_discovered[i].mac[5]);
            ESP_LOGI(TAG, "    RSSI: %d dBm, Name: %s",
                     s_discovered[i].rssi, s_discovered[i].name[0] ? s_discovered[i].name : "(no name)");
        }
    }

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
            ESP_LOGI(TAG, "  Auto-registered sensor %d: %s", s_active_sensor_count, slot->name);
            s_active_sensor_count++;
        }
    }

    ESP_LOGI(TAG, "Total active sensors: %d", s_active_sensor_count);
}

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

static bool parse_inkbird_data(const uint8_t *data, size_t len, uint8_t sensor_idx)
{
    if (sensor_idx >= INKBIRD_SENSOR_COUNT) {
        return false;
    }

    ESP_LOGI(TAG, "=== PARSING DATA ===");
    ESP_LOGI(TAG, "Length: %d bytes", len);

    if (len >= 4 && data[0] == 0x55 && data[1] == 0xAA) {
        uint8_t cmd_id = data[2];

        if (cmd_id == 0x08 && len >= 5) {
            ESP_LOGI(TAG, "Pairing response: 0x%02X", data[4]);
            return false;
        }

        if (cmd_id == 0x02 && len >= 6 && s_ble_mutex != NULL) {
            bool use_custom = (data[5] != 0x00);
            xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
            s_thresholds[sensor_idx].use_custom = use_custom;
            s_thresholds[sensor_idx].settings_valid = true;
            xSemaphoreGive(s_ble_mutex);
            ESP_LOGI(TAG, "CO2 mode: %s", use_custom ? "CUSTOM" : "DEFAULT");
            return false;
        }

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
            ESP_LOGI(TAG, "Thresholds: normal=%u-%u, plant=%u-%u ppm",
                     norm_low, norm_high, plant_low, plant_high);
            return false;
        }

        if (cmd_id != 0x01 || len < 13) {
            return false;
        }

        ESP_LOGI(TAG, "Parsing real-time data (cmd=0x01)");

        xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
        inkbird_reading_t *reading = &s_readings[sensor_idx];

        bool is_negative = (data[4] & 0x0F) != 0;
        uint16_t temp_raw = ((uint16_t)data[5] << 8) | data[6];
        reading->temperature = is_negative ? -(int16_t)temp_raw : (int16_t)temp_raw;
        reading->humidity = ((uint16_t)data[7] << 8) | data[8];
        reading->co2_ppm = ((uint16_t)data[9] << 8) | data[10];
        reading->pressure = ((uint16_t)data[11] << 8) | data[12];
        reading->timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
        reading->valid = true;
        reading->stale = false;

        xSemaphoreGive(s_ble_mutex);

        ESP_LOGI(TAG, "=== SENSOR DATA ===");
        ESP_LOGI(TAG, "  CO2: %u ppm", reading->co2_ppm);
        ESP_LOGI(TAG, "  Temperature: %.1f C", reading->temperature / 10.0f);
        ESP_LOGI(TAG, "  Humidity: %.1f%%", reading->humidity / 10.0f);
        ESP_LOGI(TAG, "  Pressure: %u hPa", reading->pressure);
        return true;
    }

    ESP_LOGW(TAG, "Unknown data format");
    return false;
}

static void read_task(void *arg)
{
    ESP_LOGI(TAG, "Read task started");

    while (s_running) {
        for (int i = 0; i < s_active_sensor_count && s_running; i++) {
            if (!s_active_sensors[i].enabled) {
                continue;
            }

            if (s_skip_cycles[i] > 0) {
                s_skip_cycles[i]--;
                continue;
            }

            ESP_LOGI(TAG, "Reading sensor %d: %s", i, s_active_sensors[i].name);

            if (!connect_to_sensor(i)) {
                s_failure_count[i]++;
                if (s_failure_count[i] >= INKBIRD_MAX_FAILURES) {
                    s_skip_cycles[i] = INKBIRD_SKIP_CYCLES_ON_FAILURE;
                    s_failure_count[i] = 0;
                }
                continue;
            }

            BaseType_t got_data = xSemaphoreTake(s_read_complete_sem,
                                                  pdMS_TO_TICKS(INKBIRD_CONNECT_TIMEOUT_MS));

            if (got_data == pdTRUE && s_data_received) {
                ESP_LOGI(TAG, "Successfully read sensor %d", i);
                s_failure_count[i] = 0;
            } else {
                ESP_LOGW(TAG, "Timeout or no data from sensor %d", i);
                s_failure_count[i]++;
                if (s_failure_count[i] >= INKBIRD_MAX_FAILURES) {
                    s_skip_cycles[i] = INKBIRD_SKIP_CYCLES_ON_FAILURE;
                    s_failure_count[i] = 0;
                }
            }

            if (s_connected) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            vTaskDelay(pdMS_TO_TICKS(INKBIRD_INTER_SENSOR_DELAY_MS));
        }

        if (s_running) {
            ESP_LOGI(TAG, "Read cycle complete, next in %d seconds",
                     INKBIRD_READ_INTERVAL_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(INKBIRD_READ_INTERVAL_MS));
        }
    }

    ESP_LOGI(TAG, "Read task exiting");
    vTaskDelete(NULL);
}

static bool is_empty_record(const uint8_t *data)
{
    for (int i = 0; i < 10; i++) {
        if (data[i] != HISTORY_EMPTY_BYTE) {
            return false;
        }
    }
    return true;
}

static bool parse_history_record(const uint8_t *data)
{
    if (is_empty_record(data)) {
        return false;
    }

    if (s_history_records == NULL || s_history_max_records == 0) {
        return false;
    }

    uint16_t co2_ppm = ((uint16_t)data[0] << 8) | data[1];

    if (co2_ppm < 200 || co2_ppm > 10000) {
        return false;
    }

    inkbird_history_record_t *rec = &s_history_records[s_history_write_idx];

    rec->co2_ppm = co2_ppm;
    rec->is_fahrenheit = (data[2] & 0xF0) != 0;
    bool is_negative = (data[2] & 0x0F) != 0;

    uint16_t temp_raw = ((uint16_t)data[3] << 8) | data[4];
    rec->temperature = is_negative ? -(int16_t)temp_raw : (int16_t)temp_raw;
    rec->humidity = ((uint16_t)data[5] << 8) | data[6];
    rec->pressure = ((uint16_t)data[7] << 8) | data[8];
    rec->interval_mins = data[9];

    s_history_received_count++;

    s_history_write_idx++;
    if (s_history_write_idx >= s_history_max_records) {
        s_history_write_idx = 0;
        s_history_buffer_wrapped = true;
    }

    return true;
}

static void parse_history_notification(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == HISTORY_END_MARKER_HIGH && data[i + 1] == HISTORY_END_MARKER_LOW) {
            ESP_LOGI(TAG, "History end marker found");
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

    if (s_history_state == INKBIRD_HISTORY_REQUESTING && !s_history_got_count) {
        if (len >= 2) {
            s_history_expected_count = ((uint16_t)data[0] << 8) | data[1];
            s_history_got_count = true;
            s_history_state = INKBIRD_HISTORY_RECEIVING;
            ESP_LOGI(TAG, "History record count: %u", s_history_expected_count);

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

    if (s_history_state == INKBIRD_HISTORY_RECEIVING) {
        if (len + s_history_buffer_len < sizeof(s_history_buffer)) {
            memcpy(s_history_buffer + s_history_buffer_len, data, len);
            s_history_buffer_len += len;
        }

        while (s_history_buffer_len >= 10) {
            if (s_history_buffer[0] == HISTORY_END_MARKER_HIGH &&
                s_history_buffer[1] == HISTORY_END_MARKER_LOW) {
                s_history_state = INKBIRD_HISTORY_COMPLETE;
                if (s_history_complete_sem != NULL) {
                    xSemaphoreGive(s_history_complete_sem);
                }
                return;
            }

            parse_history_record(s_history_buffer);

            memmove(s_history_buffer, s_history_buffer + 10, s_history_buffer_len - 10);
            s_history_buffer_len -= 10;

            if (s_history_received_count > 0 && s_history_received_count % 5000 == 0) {
                ESP_LOGI(TAG, "Progress: %u records...", s_history_received_count);
            }
        }

        if (s_history_buffer_len >= 2) {
            for (size_t i = 0; i <= s_history_buffer_len - 2; i++) {
                if (s_history_buffer[i] == HISTORY_END_MARKER_HIGH &&
                    s_history_buffer[i + 1] == HISTORY_END_MARKER_LOW) {
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

esp_err_t inkbird_ble_download_history(uint8_t sensor_idx, inkbird_history_record_t *records,
                                        uint16_t max_records, uint16_t *out_count)
{
    if (!s_ble_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (records == NULL || max_records == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sensor_idx >= s_active_sensor_count || !s_active_sensors[sensor_idx].enabled) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_history_complete_sem == NULL) {
        s_history_complete_sem = xSemaphoreCreateBinary();
        if (s_history_complete_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

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

    ESP_LOGI(TAG, "Starting history download for sensor %d", sensor_idx);

    while (xSemaphoreTake(s_read_complete_sem, 0) == pdTRUE) {}

    if (!connect_to_sensor(sensor_idx)) {
        s_history_state = INKBIRD_HISTORY_ERROR;
        return ESP_FAIL;
    }

    BaseType_t got_sem = xSemaphoreTake(s_read_complete_sem, pdMS_TO_TICKS(30000));
    if (got_sem != pdTRUE || !s_connected) {
        s_history_state = INKBIRD_HISTORY_ERROR;
        if (s_connected) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Sending history start command...");
    int rc = ble_gattc_write_flat(s_conn_handle, s_cmd_char_handle,
                                   CMD_HISTORY_START, sizeof(CMD_HISTORY_START), NULL, NULL);
    if (rc != 0) {
        s_history_state = INKBIRD_HISTORY_ERROR;
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Waiting for history data (timeout: 300s)...");
    got_sem = xSemaphoreTake(s_history_complete_sem, pdMS_TO_TICKS(300000));

    if (got_sem != pdTRUE) {
        s_history_state = INKBIRD_HISTORY_ERROR;
    }

    if (s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (s_history_buffer_wrapped) {
        s_history_stored_count = s_history_max_records;

        inkbird_history_record_t *temp = pvPortMalloc(sizeof(inkbird_history_record_t) * s_history_max_records);
        if (temp != NULL) {
            uint16_t first_part = s_history_max_records - s_history_write_idx;
            memcpy(temp, &records[s_history_write_idx], sizeof(inkbird_history_record_t) * first_part);
            memcpy(&temp[first_part], records, sizeof(inkbird_history_record_t) * s_history_write_idx);
            memcpy(records, temp, sizeof(inkbird_history_record_t) * s_history_max_records);
            vPortFree(temp);
        }
    } else {
        s_history_stored_count = s_history_write_idx;
    }

    *out_count = s_history_stored_count;

    ESP_LOGI(TAG, "History download: %u records stored", s_history_stored_count);

    bool success = (s_history_state == INKBIRD_HISTORY_COMPLETE ||
        (s_history_stored_count > 0 && s_history_received_count > s_history_expected_count * 9 / 10));

    s_history_state = INKBIRD_HISTORY_IDLE;
    s_history_records = NULL;

    return success ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t inkbird_ble_cancel_history(void)
{
    if (s_history_state == INKBIRD_HISTORY_IDLE) {
        return ESP_OK;
    }

    if (s_connected && s_cmd_char_handle != 0) {
        ble_gattc_write_flat(s_conn_handle, s_cmd_char_handle,
                              CMD_HISTORY_STOP, sizeof(CMD_HISTORY_STOP), NULL, NULL);
    }

    s_history_state = INKBIRD_HISTORY_IDLE;
    s_history_records = NULL;
    s_history_buffer_len = 0;

    return ESP_OK;
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

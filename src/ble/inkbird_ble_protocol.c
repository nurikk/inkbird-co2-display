/**
 * @file inkbird_ble_protocol.c
 * @brief BLE protocol handling for Inkbird sensors (NimBLE)
 *
 * Handles GAP events (scan, connect, disconnect) and GATT operations
 * (service discovery, characteristic discovery, notifications).
 */

#include "inkbird_ble_internal.h"

static const char *TAG = "inkbird_proto";

// Forward declarations
static void inkbird_on_connect(inkbird_peer_t *peer, uint16_t conn_handle);
static void inkbird_on_disconnect(inkbird_peer_t *peer, int reason);
static void inkbird_start_service_discovery(inkbird_peer_t *peer);
static void inkbird_discover_chars(inkbird_peer_t *peer);
static void inkbird_discover_descs(inkbird_peer_t *peer);
static void inkbird_enable_notifications(inkbird_peer_t *peer);
static void inkbird_send_pairing(inkbird_peer_t *peer);

// ============================================================================
// Scan Handling
// ============================================================================

static bool is_inkbird_device(const struct ble_hs_adv_fields *fields)
{
    // Check if device advertises the Inkbird service UUID (0xFFE0)
    for (int i = 0; i < fields->num_uuids16; i++) {
        if (ble_uuid_u16(&fields->uuids16[i].u) == INKBIRD_SVC_UUID16) {
            return true;
        }
    }
    return false;
}

static bool is_inkbird_name(const char *name, size_t len)
{
    if (name == NULL || len == 0) return false;

    // Only match "Ink@IAM-T1" sensor name
    if (len >= 10 && strncasecmp(name, "Ink@IAM-T1", 10) == 0) return true;

    return false;
}

static void on_scan_result(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    int rc;

    // Parse advertisement data
    rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (rc != 0) {
        return;
    }

    // Check if this is an Inkbird device - must match name "Ink@IAM-T1"
    // (UUID 0xFFE0 is too generic, many devices use it)
    if (fields.name == NULL || !is_inkbird_name((const char *)fields.name, fields.name_len)) {
        return;
    }

    // Check if already discovered
    for (int i = 0; i < s_discovered_count; i++) {
        if (memcmp(s_discovered[i].mac, disc->addr.val, 6) == 0) {
            return;  // Already known
        }
    }

    // Check for space
    if (s_discovered_count >= INKBIRD_MAX_DISCOVERED) {
        return;
    }

    // Add to discovered list
    inkbird_discovered_t *d = &s_discovered[s_discovered_count];
    memcpy(d->mac, disc->addr.val, 6);
    d->addr_type = disc->addr.type;
    d->rssi = disc->rssi;

    // Copy name if available
    if (fields.name != NULL) {
        size_t name_len = fields.name_len < sizeof(d->name) - 1 ?
                          fields.name_len : sizeof(d->name) - 1;
        memcpy(d->name, fields.name, name_len);
        d->name[name_len] = '\0';
    } else {
        d->name[0] = '\0';
    }

    ESP_LOGI(TAG, "Discovered Inkbird: %02X:%02X:%02X:%02X:%02X:%02X RSSI=%d Name=%s",
             d->mac[0], d->mac[1], d->mac[2],
             d->mac[3], d->mac[4], d->mac[5],
             d->rssi, d->name);

    s_discovered_count++;
}

// ============================================================================
// GAP Event Handler
// ============================================================================

int inkbird_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    inkbird_peer_t *peer;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        // Scan result
        on_scan_result(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete");
        s_scanning = false;
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected (handle=%d)", event->connect.conn_handle);

            // Find peer by address or assign to first waiting peer
            peer = NULL;
            for (int i = 0; i < s_peer_count; i++) {
                // Connection established - assign handle to first waiting peer
                if (!s_peers[i].connected && s_peers[i].conn_handle == INVALID_CONN_HANDLE) {
                    peer = &s_peers[i];
                    break;
                }
            }

            if (peer != NULL) {
                inkbird_on_connect(peer, event->connect.conn_handle);
            } else {
                ESP_LOGW(TAG, "No peer waiting for connection");
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
            // Don't retry here - let the higher level code handle retries
            // to avoid connection limit errors from rapid retry attempts
            if (s_read_complete_sem != NULL) {
                xSemaphoreGive(s_read_complete_sem);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (handle=%d, reason=%d)",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);

        peer = peer_find_by_conn_handle(event->disconnect.conn.conn_handle);
        if (peer != NULL) {
            inkbird_on_disconnect(peer, event->disconnect.reason);
        }

        s_connected = false;

        // Signal semaphore in case someone is waiting
        if (s_read_complete_sem != NULL) {
            xSemaphoreGive(s_read_complete_sem);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        peer = peer_find_by_conn_handle(event->notify_rx.conn_handle);
        if (peer != NULL) {
            uint8_t sensor_idx = peer->sensor_idx;
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);

            if (len > sizeof(peer->recv_data)) {
                len = sizeof(peer->recv_data);
            }

            os_mbuf_copydata(event->notify_rx.om, 0, len, peer->recv_data);
            peer->recv_len = len;

            ESP_LOGD(TAG, "Notification from sensor %d: %d bytes", sensor_idx, len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, peer->recv_data, len, ESP_LOG_DEBUG);

            // Check if this is history data
            if (s_history_state == INKBIRD_HISTORY_REQUESTING ||
                s_history_state == INKBIRD_HISTORY_RECEIVING) {
                inkbird_parse_history_notification(peer->recv_data, len);
            } else {
                // Normal data parsing
                if (inkbird_parse_data(peer->recv_data, len, sensor_idx)) {
                    peer->data_received = true;
                    s_data_received = true;

                    // Update legacy globals
                    s_current_sensor_index = sensor_idx;
                    memcpy(s_recv_data, peer->recv_data, len);
                    s_recv_len = len;

                    // Signal completion for one-shot reads
                    if (s_read_complete_sem != NULL) {
                        xSemaphoreGive(s_read_complete_sem);
                    }
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe event: handle=%d, attr=%d, cur_notify=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection params updated: handle=%d",
                 event->conn_update.conn_handle);
        return 0;

    default:
        ESP_LOGD(TAG, "GAP event: %d", event->type);
        return 0;
    }
}

// ============================================================================
// Scan Start/Stop
// ============================================================================

void inkbird_start_scan(void)
{
    struct ble_gap_disc_params disc_params = {
        .itvl = 0,               // Use default
        .window = 0,             // Use default
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,            // Active scanning
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(s_own_addr_type, INKBIRD_SCAN_DURATION_SEC * 1000,
                          &disc_params, inkbird_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
    } else {
        s_scanning = true;
        ESP_LOGI(TAG, "Scanning started...");
    }
}

void inkbird_stop_scan(void)
{
    ble_gap_disc_cancel();
    s_scanning = false;
    ESP_LOGI(TAG, "Scanning stopped");
}

// ============================================================================
// Connection Management
// ============================================================================

void inkbird_start_connect(inkbird_peer_t *peer)
{
    if (peer == NULL) {
        ESP_LOGE(TAG, "Cannot connect: peer is NULL");
        return;
    }

    // Stop scanning if active
    if (s_scanning) {
        inkbird_stop_scan();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Cancel any ongoing connection attempt and wait for it to complete
    int cancel_attempts = 0;
    while (cancel_attempts < 5) {
        int rc = ble_gap_conn_cancel();
        if (rc == BLE_HS_ENOENT) {
            // No connection in progress, we can proceed
            break;
        } else if (rc == 0) {
            // Cancel initiated, wait for it to complete
            ESP_LOGD(TAG, "Cancelled previous connection attempt");
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            // Other error, try again
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        cancel_attempts++;
    }

    struct ble_gap_conn_params conn_params = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
        .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = 0x0200,  // 5.12 seconds
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
             peer->remote_addr.val[5], peer->remote_addr.val[4],
             peer->remote_addr.val[3], peer->remote_addr.val[2],
             peer->remote_addr.val[1], peer->remote_addr.val[0],
             peer->remote_addr.type);

    int rc = ble_gap_connect(s_own_addr_type, &peer->remote_addr,
                             30000,  // 30 second timeout
                             &conn_params,
                             inkbird_gap_event_handler, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate connection: %d", rc);
        if (s_read_complete_sem != NULL) {
            xSemaphoreGive(s_read_complete_sem);
        }
    }
}

static void inkbird_on_connect(inkbird_peer_t *peer, uint16_t conn_handle)
{
    peer->conn_handle = conn_handle;
    peer->connected = true;
    peer->data_received = false;

    // Update legacy globals
    s_connected = true;
    s_conn_handle = conn_handle;

    ESP_LOGI(TAG, "Sensor %d (%s) connected, discovering services...",
             peer->sensor_idx, s_active_sensors[peer->sensor_idx].name);

    sensor_data_set_activity_status(peer->sensor_idx, "Discovering...");

    // Start service discovery
    inkbird_start_service_discovery(peer);
}

static void inkbird_on_disconnect(inkbird_peer_t *peer, int reason)
{
    ESP_LOGI(TAG, "Sensor %d disconnected (reason=%d)",
             peer->sensor_idx, reason);

    peer->connected = false;
    peer->ready = false;
    peer->conn_handle = INVALID_CONN_HANDLE;

    // Clear GATT handles
    peer->service_start_handle = 0;
    peer->service_end_handle = 0;
    peer->data_char_handle = 0;
    peer->data_char_val_handle = 0;
    peer->cmd_char_handle = 0;
    peer->cmd_char_val_handle = 0;
    peer->cccd_handle = 0;
}

// ============================================================================
// Service Discovery
// ============================================================================

static void inkbird_start_service_discovery(inkbird_peer_t *peer)
{
    ble_uuid16_t svc_uuid;
    svc_uuid.u.type = BLE_UUID_TYPE_16;
    svc_uuid.value = INKBIRD_SVC_UUID16;

    int rc = ble_gattc_disc_svc_by_uuid(peer->conn_handle,
                                         &svc_uuid.u,
                                         inkbird_on_disc_complete,
                                         peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Service discovery failed: %d", rc);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int inkbird_on_disc_complete(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg)
{
    inkbird_peer_t *peer = (inkbird_peer_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Service discovery complete for sensor %d", peer->sensor_idx);
        if (peer->service_start_handle != 0) {
            inkbird_discover_chars(peer);
        } else {
            ESP_LOGE(TAG, "Service 0x%04X not found", INKBIRD_SVC_UUID16);
            ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery error: %d", error->status);
        return 0;
    }

    // Found service
    peer->service_start_handle = service->start_handle;
    peer->service_end_handle = service->end_handle;

    ESP_LOGI(TAG, "Found service 0x%04X: handles %d-%d",
             INKBIRD_SVC_UUID16,
             peer->service_start_handle,
             peer->service_end_handle);

    // Update legacy globals
    s_service_start_handle = peer->service_start_handle;
    s_service_end_handle = peer->service_end_handle;

    return 0;
}

// ============================================================================
// Characteristic Discovery
// ============================================================================

static void inkbird_discover_chars(inkbird_peer_t *peer)
{
    sensor_data_set_activity_status(peer->sensor_idx, "Scanning...");

    int rc = ble_gattc_disc_all_chrs(peer->conn_handle,
                                      peer->service_start_handle,
                                      peer->service_end_handle,
                                      inkbird_on_chr_disc,
                                      peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Characteristic discovery failed: %d", rc);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int inkbird_on_chr_disc(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr,
                         void *arg)
{
    inkbird_peer_t *peer = (inkbird_peer_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Characteristic discovery complete");

        if (peer->data_char_handle != 0) {
            // Discover descriptors for the data characteristic
            inkbird_discover_descs(peer);
        } else {
            ESP_LOGE(TAG, "Data characteristic 0x%04X not found", INKBIRD_DATA_UUID16);
            ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
        return 0;
    }

    // Check UUID
    if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
        uint16_t uuid16 = BLE_UUID16(&chr->uuid)->value;

        if (uuid16 == INKBIRD_DATA_UUID16) {
            peer->data_char_handle = chr->def_handle;
            peer->data_char_val_handle = chr->val_handle;
            s_data_char_handle = chr->val_handle;

            ESP_LOGI(TAG, "Found data char 0x%04X: def=%d val=%d",
                     uuid16, chr->def_handle, chr->val_handle);
        } else if (uuid16 == INKBIRD_CMD_UUID16) {
            peer->cmd_char_handle = chr->def_handle;
            peer->cmd_char_val_handle = chr->val_handle;
            s_cmd_char_handle = chr->val_handle;

            ESP_LOGI(TAG, "Found cmd char 0x%04X: def=%d val=%d",
                     uuid16, chr->def_handle, chr->val_handle);
        }
    }

    return 0;
}

// ============================================================================
// Descriptor Discovery
// ============================================================================

static void inkbird_discover_descs(inkbird_peer_t *peer)
{
    uint16_t start = peer->data_char_val_handle + 1;
    uint16_t end = peer->service_end_handle;

    // If command char exists and comes after data char, use its handle as end
    if (peer->cmd_char_handle > peer->data_char_val_handle) {
        end = peer->cmd_char_handle - 1;
    }

    ESP_LOGI(TAG, "Discovering descriptors: handles %d-%d", start, end);

    int rc = ble_gattc_disc_all_dscs(peer->conn_handle,
                                      start, end,
                                      inkbird_on_dsc_disc,
                                      peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Descriptor discovery failed: %d", rc);
        // Continue anyway - try to enable notifications
        inkbird_enable_notifications(peer);
    }
}

int inkbird_on_dsc_disc(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc,
                         void *arg)
{
    inkbird_peer_t *peer = (inkbird_peer_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Descriptor discovery complete");

        if (peer->cccd_handle != 0) {
            inkbird_enable_notifications(peer);
        } else {
            ESP_LOGW(TAG, "CCCD not found, trying notifications anyway");
            inkbird_enable_notifications(peer);
        }
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Descriptor discovery error: %d", error->status);
        return 0;
    }

    // Check if this is CCCD
    if (dsc->uuid.u.type == BLE_UUID_TYPE_16) {
        uint16_t uuid16 = BLE_UUID16(&dsc->uuid)->value;
        if (uuid16 == BLE_GATT_DSC_CLT_CFG_UUID16) {
            peer->cccd_handle = dsc->handle;
            s_cccd_handle = dsc->handle;
            ESP_LOGI(TAG, "Found CCCD: handle=%d", dsc->handle);
        }
    }

    return 0;
}

// ============================================================================
// Notifications
// ============================================================================

static int inkbird_on_subscribe_cb(uint16_t conn_handle,
                                    const struct ble_gatt_error *error,
                                    struct ble_gatt_attr *attr,
                                    void *arg);

static void inkbird_enable_notifications(inkbird_peer_t *peer)
{
    uint8_t value[2] = {0x01, 0x00};  // Enable notifications

    uint16_t cccd = peer->cccd_handle;
    if (cccd == 0) {
        // Guess CCCD handle (usually value_handle + 1)
        cccd = peer->data_char_val_handle + 1;
        ESP_LOGW(TAG, "CCCD not found, guessing handle=%d", cccd);
    }

    sensor_data_set_activity_status(peer->sensor_idx, "Subscribing...");
    ESP_LOGI(TAG, "Enabling notifications on CCCD handle=%d", cccd);

    int rc = ble_gattc_write_flat(peer->conn_handle,
                                   cccd, value, sizeof(value),
                                   inkbird_on_subscribe_cb, peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to write CCCD: %d", rc);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int inkbird_on_subscribe_cb(uint16_t conn_handle,
                                    const struct ble_gatt_error *error,
                                    struct ble_gatt_attr *attr,
                                    void *arg)
{
    inkbird_peer_t *peer = (inkbird_peer_t *)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "Notification subscription failed: %d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    ESP_LOGI(TAG, "Notifications enabled for sensor %d", peer->sensor_idx);
    peer->ready = true;

    // If in history setup mode, signal ready and let caller send history command
    if (s_history_setup_mode) {
        ESP_LOGI(TAG, "History setup complete, signaling ready");
        if (s_read_complete_sem != NULL) {
            xSemaphoreGive(s_read_complete_sem);
        }
        return 0;
    }

    // Otherwise, send pairing command to start data flow
    inkbird_send_pairing(peer);

    return 0;
}

int inkbird_on_subscribe(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr,
                          void *arg)
{
    return inkbird_on_subscribe_cb(conn_handle, error, attr, arg);
}

// ============================================================================
// Commands
// ============================================================================

static int inkbird_on_write_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr,
                                void *arg);

static void inkbird_send_pairing(inkbird_peer_t *peer)
{
    if (peer->cmd_char_val_handle == 0) {
        ESP_LOGW(TAG, "No command characteristic, waiting for notifications...");
        return;
    }

    sensor_data_set_activity_status(peer->sensor_idx, "Pairing...");
    ESP_LOGI(TAG, "Sending pairing command to sensor %d", peer->sensor_idx);

    int rc = ble_gattc_write_flat(peer->conn_handle,
                                   peer->cmd_char_val_handle,
                                   CMD_PAIRING, sizeof(CMD_PAIRING),
                                   inkbird_on_write_cb, peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to send pairing command: %d", rc);
    }
}

static int inkbird_on_write_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr,
                                void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "Write failed: %d", error->status);
    } else {
        ESP_LOGD(TAG, "Write successful");
    }
    return 0;
}

int inkbird_on_write(uint16_t conn_handle,
                      const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr,
                      void *arg)
{
    return inkbird_on_write_cb(conn_handle, error, attr, arg);
}

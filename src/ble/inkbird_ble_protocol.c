/**
 * @file inkbird_ble_protocol.c
 * @brief BLE GAP and GATT event handlers for Inkbird sensors
 *
 * Handles all BLE protocol events including:
 * - GAP scanning and device discovery
 * - GATT connection, service discovery, and characteristic operations
 * - Notification registration and data reception
 *
 * Multi-connection support: Events are routed to the correct peer
 * based on conn_id or MAC address.
 */

#include "inkbird_ble_internal.h"

static const char *TAG = "inkbird_protocol";

// ============================================================================
// GAP Event Handler
// ============================================================================

void inkbird_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
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
// GATTC Event Handler (Multi-Connection)
// ============================================================================

void inkbird_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    inkbird_peer_t *peer = NULL;

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
            ESP_LOGI(TAG, "Connected, conn_id: %d", param->connect.conn_id);
            break;

        case ESP_GATTC_OPEN_EVT:
            // Find peer by MAC address (conn_id not yet assigned to peer)
            peer = peer_find_by_mac(param->open.remote_bda);
            if (peer == NULL) {
                ESP_LOGW(TAG, "Open event for unknown device, ignoring");
                break;
            }

            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Open failed for sensor %d, status: %d",
                         peer->sensor_idx, param->open.status);
                peer->connected = false;
                sensor_data_set_status(peer->sensor_idx, "Connect failed");
                xSemaphoreGive(s_read_complete_sem);
            } else {
                ESP_LOGI(TAG, "Open success, conn_id: %d, sensor: %d (%s)",
                         param->open.conn_id, peer->sensor_idx,
                         s_active_sensors[peer->sensor_idx].name);

                peer->conn_id = param->open.conn_id;
                peer->connected = true;
                sensor_data_set_status(peer->sensor_idx, "Discovering...");

                // Reset GATT handles for this peer
                peer->service_start_handle = 0;
                peer->service_end_handle = 0;
                peer->data_char_handle = 0;
                peer->cmd_char_handle = 0;
                peer->cccd_handle = 0;

                // Request MTU
                esp_err_t ret = esp_ble_gattc_send_mtu_req(gattc_if, peer->conn_id);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "MTU request failed: %s", esp_err_to_name(ret));
                    // Continue anyway - discover services
                    esp_ble_gattc_search_service(gattc_if, peer->conn_id, NULL);
                }
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            peer = peer_find_by_conn_id(param->cfg_mtu.conn_id);
            if (peer == NULL) break;

            if (param->cfg_mtu.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "MTU config failed: %d", param->cfg_mtu.status);
            } else {
                ESP_LOGI(TAG, "MTU configured: %d for sensor %d",
                         param->cfg_mtu.mtu, peer->sensor_idx);
            }
            // Discover services
            esp_ble_gattc_search_service(gattc_if, peer->conn_id, NULL);
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            peer = peer_find_by_conn_id(param->search_res.conn_id);
            if (peer == NULL) break;

            ESP_LOGD(TAG, "Service found: UUID 0x%04X, start: %d, end: %d (sensor %d)",
                     param->search_res.srvc_id.uuid.uuid.uuid16,
                     param->search_res.start_handle,
                     param->search_res.end_handle,
                     peer->sensor_idx);

            if (param->search_res.srvc_id.uuid.uuid.uuid16 == INKBIRD_SVC_UUID16) {
                ESP_LOGI(TAG, "Found Inkbird FFE0 service for sensor %d", peer->sensor_idx);
                peer->service_start_handle = param->search_res.start_handle;
                peer->service_end_handle = param->search_res.end_handle;
            }
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT: {
            peer = peer_find_by_conn_id(param->search_cmpl.conn_id);
            if (peer == NULL) break;

            if (param->search_cmpl.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Service search failed: %d", param->search_cmpl.status);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            ESP_LOGI(TAG, "Service discovery complete for sensor %d", peer->sensor_idx);

            if (peer->service_start_handle == 0) {
                ESP_LOGW(TAG, "Inkbird service not found for sensor %d!", peer->sensor_idx);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            // Get all characteristics in the Inkbird service
            uint16_t count = 0;
            esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                gattc_if, peer->conn_id, ESP_GATT_DB_CHARACTERISTIC,
                peer->service_start_handle, peer->service_end_handle,
                INVALID_HANDLE, &count);

            if (status != ESP_GATT_OK || count == 0) {
                ESP_LOGW(TAG, "No characteristics found for sensor %d", peer->sensor_idx);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            ESP_LOGI(TAG, "Found %d characteristics for sensor %d", count, peer->sensor_idx);

            esp_gattc_char_elem_t *char_elem = malloc(sizeof(esp_gattc_char_elem_t) * count);
            if (char_elem == NULL) {
                ESP_LOGE(TAG, "malloc failed");
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            status = esp_ble_gattc_get_all_char(
                gattc_if, peer->conn_id,
                peer->service_start_handle, peer->service_end_handle,
                char_elem, &count, 0);

            if (status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Get all chars failed: %d", status);
                free(char_elem);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            for (int i = 0; i < count; i++) {
                uint16_t uuid16 = char_elem[i].uuid.uuid.uuid16;
                ESP_LOGD(TAG, "Char %d: UUID=0x%04X handle=%d props=0x%02X",
                         i, uuid16, char_elem[i].char_handle, char_elem[i].properties);

                if (uuid16 == INKBIRD_DATA_UUID16) {
                    peer->data_char_handle = char_elem[i].char_handle;
                    ESP_LOGI(TAG, "Data char FFE4: handle=%d (sensor %d)",
                             peer->data_char_handle, peer->sensor_idx);
                } else if (uuid16 == INKBIRD_CMD_UUID16) {
                    peer->cmd_char_handle = char_elem[i].char_handle;
                    ESP_LOGI(TAG, "Cmd char FFE9: handle=%d (sensor %d)",
                             peer->cmd_char_handle, peer->sensor_idx);
                }
            }
            free(char_elem);

            if (peer->data_char_handle == 0) {
                ESP_LOGW(TAG, "Data characteristic FFE4 not found for sensor %d!",
                         peer->sensor_idx);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            // Get descriptors for data characteristic
            count = 0;
            status = esp_ble_gattc_get_attr_count(
                gattc_if, peer->conn_id, ESP_GATT_DB_DESCRIPTOR,
                peer->service_start_handle, peer->service_end_handle,
                peer->data_char_handle, &count);

            ESP_LOGD(TAG, "Found %d descriptors for FFE4 (sensor %d)", count, peer->sensor_idx);

            if (count > 0) {
                esp_gattc_descr_elem_t *descr_elem = malloc(sizeof(esp_gattc_descr_elem_t) * count);
                if (descr_elem != NULL) {
                    status = esp_ble_gattc_get_all_descr(
                        gattc_if, peer->conn_id,
                        peer->data_char_handle,
                        descr_elem, &count, 0);

                    if (status == ESP_GATT_OK) {
                        for (int i = 0; i < count; i++) {
                            if (descr_elem[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                                peer->cccd_handle = descr_elem[i].handle;
                                ESP_LOGI(TAG, "CCCD handle: %d (sensor %d)",
                                         peer->cccd_handle, peer->sensor_idx);
                            }
                        }
                    }
                    free(descr_elem);
                }
            }

            // Register for notifications
            if (peer->data_char_handle != 0) {
                ESP_LOGI(TAG, "Registering for notifications on FFE4 (sensor %d)...",
                         peer->sensor_idx);
                esp_err_t ret = esp_ble_gattc_register_for_notify(
                    gattc_if, peer->remote_bda, peer->data_char_handle);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Register for notify failed: %s", esp_err_to_name(ret));
                    xSemaphoreGive(s_read_complete_sem);
                }
            }
            break;
        }

        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Register for notify failed: %d", param->reg_for_notify.status);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }

            // Find peer by the handle we registered
            for (int i = 0; i < s_peer_count; i++) {
                if (s_peers[i].data_char_handle == param->reg_for_notify.handle) {
                    peer = &s_peers[i];
                    break;
                }
            }
            if (peer == NULL) {
                ESP_LOGW(TAG, "REG_FOR_NOTIFY for unknown handle %d", param->reg_for_notify.handle);
                break;
            }

            ESP_LOGI(TAG, "Registered for notify, handle: %d (sensor %d)",
                     param->reg_for_notify.handle, peer->sensor_idx);
            sensor_data_set_status(peer->sensor_idx, "Subscribing...");

            // Write CCCD to enable notifications (0x0001)
            if (peer->cccd_handle != 0) {
                uint16_t notify_enable = 0x0001;
                esp_err_t ret = esp_ble_gattc_write_char_descr(
                    gattc_if, peer->conn_id, peer->cccd_handle,
                    sizeof(notify_enable), (uint8_t *)&notify_enable,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Write CCCD failed: %s", esp_err_to_name(ret));
                    xSemaphoreGive(s_read_complete_sem);
                } else {
                    ESP_LOGI(TAG, "CCCD write initiated (sensor %d)", peer->sensor_idx);
                }
            } else {
                // No CCCD found, try writing to handle+1
                ESP_LOGW(TAG, "No CCCD found for sensor %d, trying handle+1", peer->sensor_idx);
                uint16_t notify_enable = 0x0001;
                esp_ble_gattc_write_char_descr(
                    gattc_if, peer->conn_id, peer->data_char_handle + 1,
                    sizeof(notify_enable), (uint8_t *)&notify_enable,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            }
            break;
        }

        case ESP_GATTC_WRITE_DESCR_EVT:
            peer = peer_find_by_conn_id(param->write.conn_id);
            if (peer == NULL) break;

            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Write descriptor failed: %d (sensor %d)",
                         param->write.status, peer->sensor_idx);
                xSemaphoreGive(s_read_complete_sem);
                break;
            }
            ESP_LOGI(TAG, "CCCD write success - notifications enabled (sensor %d)",
                     peer->sensor_idx);

            // For history download mode, signal that setup is complete
            // For normal mode, send pairing request first per protocol section 7.2
            ESP_LOGI(TAG, "s_history_state=%d (IDLE=0)", s_history_state);
            if (s_history_state != INKBIRD_HISTORY_IDLE) {
                ESP_LOGI(TAG, "History mode: signaling setup complete");
                // Update legacy globals for history compatibility
                s_current_sensor_index = peer->sensor_idx;
                s_conn_id = peer->conn_id;
                s_cmd_char_handle = peer->cmd_char_handle;
                xSemaphoreGive(s_read_complete_sem);
            } else {
                sensor_data_set_status(peer->sensor_idx, "Pairing...");
                if (peer->cmd_char_handle != 0) {
                    // Send pairing request (0x08) per protocol section 7.2
                    ESP_LOGI(TAG, "Sending pairing request (sensor %d)...", peer->sensor_idx);
                    esp_ble_gattc_write_char(
                        gattc_if, peer->conn_id, peer->cmd_char_handle,
                        sizeof(CMD_PAIRING), (uint8_t *)CMD_PAIRING,
                        ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
                } else {
                    ESP_LOGW(TAG, "No command handle for sensor %d, waiting for passive notification...",
                             peer->sensor_idx);
                }
            }
            break;

        case ESP_GATTC_NOTIFY_EVT:
            peer = peer_find_by_conn_id(param->notify.conn_id);
            if (peer == NULL) {
                ESP_LOGW(TAG, "Notification for unknown conn_id %d", param->notify.conn_id);
                break;
            }

            if (param->notify.value_len > 0) {
                // Check if we're in history download mode
                if (s_history_state == INKBIRD_HISTORY_REQUESTING ||
                    s_history_state == INKBIRD_HISTORY_RECEIVING) {
                    // History mode - minimal logging
                    // Update legacy globals for history compatibility
                    s_current_sensor_index = peer->sensor_idx;
                    inkbird_parse_history_notification(param->notify.value, param->notify.value_len);
                } else {
                    // Normal mode - log details
                    ESP_LOGI(TAG, "Notification: sensor=%d, handle=%d, len=%d",
                             peer->sensor_idx, param->notify.handle, param->notify.value_len);
                    ESP_LOG_BUFFER_HEX(TAG, param->notify.value, param->notify.value_len);

                    // Copy data to peer's buffer
                    peer->recv_len = param->notify.value_len;
                    if (peer->recv_len > sizeof(peer->recv_data)) {
                        peer->recv_len = sizeof(peer->recv_data);
                    }
                    memcpy(peer->recv_data, param->notify.value, peer->recv_len);

                    // Parse data - returns true only for real-time data (cmd 0x01)
                    bool is_realtime = inkbird_parse_data(peer->recv_data, peer->recv_len,
                                                          peer->sensor_idx);

                    // Only signal completion for real-time data
                    if (is_realtime) {
                        peer->data_received = true;
                        peer->ready = true;
                        sensor_data_set_status(peer->sensor_idx, NULL);
                        xSemaphoreGive(s_read_complete_sem);
                    }
                }
            }
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            peer = peer_find_by_conn_id(param->write.conn_id);
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Write char failed: %d", param->write.status);
            } else {
                ESP_LOGI(TAG, "Command written successfully to handle %d (sensor %d)",
                         param->write.handle, peer ? peer->sensor_idx : -1);
            }
            break;

        case ESP_GATTC_CLOSE_EVT:
        case ESP_GATTC_DISCONNECT_EVT:
            peer = peer_find_by_conn_id(param->disconnect.conn_id);
            if (peer != NULL) {
                ESP_LOGI(TAG, "Disconnected sensor %d (%s)",
                         peer->sensor_idx, s_active_sensors[peer->sensor_idx].name);
                peer->connected = false;
                peer->ready = false;
            } else {
                ESP_LOGI(TAG, "Disconnected unknown conn_id %d", param->disconnect.conn_id);
            }
            xSemaphoreGive(s_read_complete_sem);
            break;

        default:
            ESP_LOGD(TAG, "GATTC event: %d", event);
            break;
    }
}

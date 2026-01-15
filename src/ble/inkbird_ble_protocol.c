/**
 * @file inkbird_ble_protocol.c
 * @brief BLE GAP and GATT event handlers for Inkbird sensors
 *
 * Handles all BLE protocol events including:
 * - GAP scanning and device discovery
 * - GATT connection, service discovery, and characteristic operations
 * - Notification registration and data reception
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
// GATTC Event Handler
// ============================================================================

void inkbird_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
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
                sensor_data_set_status(s_current_sensor_index, "Connect failed");
                xSemaphoreGive(s_read_complete_sem);
            } else {
                ESP_LOGI(TAG, "Open success, conn_id: %d", param->open.conn_id);
                s_conn_id = param->open.conn_id;
                s_connected = true;
                sensor_data_set_status(s_current_sensor_index, "Discovering...");

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
            sensor_data_set_status(s_current_sensor_index, "Subscribing...");

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

            // For history download mode, signal that setup is complete
            // For normal mode, send pairing request first per protocol section 7.2
            if (s_history_state != INKBIRD_HISTORY_IDLE) {
                ESP_LOGI(TAG, "History mode: signaling setup complete");
                xSemaphoreGive(s_read_complete_sem);
            } else {
                sensor_data_set_status(s_current_sensor_index, "Pairing...");
                if (s_cmd_char_handle != 0) {
                    // Send pairing request (0x08) per protocol section 7.2
                    ESP_LOGI(TAG, "Sending pairing request...");
                    esp_ble_gattc_write_char(
                        gattc_if, s_conn_id, s_cmd_char_handle,
                        sizeof(CMD_PAIRING), (uint8_t *)CMD_PAIRING,
                        ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
                } else {
                    ESP_LOGW(TAG, "No command handle, waiting for passive notification...");
                }
            }
            break;

        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.value_len > 0) {
                // Check if we're in history download mode
                if (s_history_state == INKBIRD_HISTORY_REQUESTING ||
                    s_history_state == INKBIRD_HISTORY_RECEIVING) {
                    // History mode - minimal logging
                    inkbird_parse_history_notification(param->notify.value, param->notify.value_len);
                } else {
                    // Normal mode - log details
                    ESP_LOGI(TAG, "Notification: handle=%d, len=%d", param->notify.handle, param->notify.value_len);
                    ESP_LOG_BUFFER_HEX(TAG, param->notify.value, param->notify.value_len);

                    // Copy data
                    s_recv_len = param->notify.value_len;
                    if (s_recv_len > RECV_DATA_BUF_SIZE) {
                        s_recv_len = RECV_DATA_BUF_SIZE;
                    }
                    memcpy(s_recv_data, param->notify.value, s_recv_len);

                    // Parse data - returns true only for real-time data (cmd 0x01)
                    bool is_realtime = inkbird_parse_data(s_recv_data, s_recv_len, s_current_sensor_index);

                    // Only signal completion for real-time data
                    if (is_realtime) {
                        s_data_received = true;
                        sensor_data_set_status(s_current_sensor_index, NULL);
                        xSemaphoreGive(s_read_complete_sem);
                    }
                }
            }
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Write char failed: %d", param->write.status);
            } else {
                ESP_LOGI(TAG, "Command written successfully to handle %d", param->write.handle);
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

/**
 * @file inkbird_ble_internal.h
 * @brief Internal shared state and declarations for Inkbird BLE modules
 *
 * This header is used internally by the inkbird_ble_* modules to share
 * state variables, constants, and forward declarations. Not intended for
 * external use - use inkbird_ble.h for the public API.
 */

#ifndef INKBIRD_BLE_INTERNAL_H
#define INKBIRD_BLE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"

#include "inkbird_ble.h"
#include "inkbird_config.h"
#include "sensor_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define GATTC_APP_ID            0
#define INVALID_HANDLE          0
#define PROFILE_NUM             1
#define PROFILE_APP_IDX         0
#define BLE_TASK_CORE           1
#define MAX_PEERS               INKBIRD_SENSOR_COUNT
#define INVALID_CONN_ID         0xFFFF

// ============================================================================
// Per-Connection State (Peer)
// ============================================================================

typedef struct {
    uint8_t sensor_idx;               // Index into s_active_sensors[]
    esp_bd_addr_t remote_bda;         // Device MAC address
    uint16_t conn_id;                 // Connection ID from stack
    bool connected;                   // Connection established
    bool ready;                       // Notifications enabled, ready for data

    // GATT handles (discovered per-connection)
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t data_char_handle;        // FFE4 - notifications
    uint16_t cmd_char_handle;         // FFE9 - commands
    uint16_t cccd_handle;

    // Data reception
    bool data_received;
    uint8_t recv_data[32];
    size_t recv_len;
} inkbird_peer_t;

// Inkbird service and characteristic UUIDs (16-bit)
#define INKBIRD_SVC_UUID16      0xFFE0
#define INKBIRD_DATA_UUID16     0xFFE4  // Notifications
#define INKBIRD_CMD_UUID16      0xFFE9  // Write commands

// CCCD UUID for enabling notifications
#define ESP_GATT_UUID_CHAR_CLIENT_CONFIG 0x2902

// History end marker
#define HISTORY_END_MARKER_HIGH 0x66
#define HISTORY_END_MARKER_LOW  0x66

// Maximum history records to store
#define INKBIRD_MAX_HISTORY_RECORDS 1000

// Empty record detection (all 0xFF means uninitialized flash)
#define HISTORY_EMPTY_BYTE 0xFF

// ============================================================================
// Command Packets (from APK reverse engineering)
// ============================================================================

// Command sizes
#define CMD_REALTIME_DATA_LEN   6
#define CMD_PAIRING_LEN         6
#define CMD_CO2_SETTINGS_LEN    11
#define CMD_CO2_THRESHOLDS_LEN  14
#define CMD_CO2_ALARM_LEN       9
#define CMD_CALIBRATION_LEN     12
#define CMD_HISTORY_START_LEN   6
#define CMD_HISTORY_STOP_LEN    6

// Receive buffer size
#define RECV_DATA_BUF_SIZE      32
#define HISTORY_BUFFER_SIZE     256

extern const uint8_t CMD_REALTIME_DATA[CMD_REALTIME_DATA_LEN];
extern const uint8_t CMD_PAIRING[CMD_PAIRING_LEN];
extern const uint8_t CMD_CO2_SETTINGS[CMD_CO2_SETTINGS_LEN];
extern const uint8_t CMD_CO2_THRESHOLDS[CMD_CO2_THRESHOLDS_LEN];
extern const uint8_t CMD_CO2_ALARM[CMD_CO2_ALARM_LEN];
extern const uint8_t CMD_CALIBRATION[CMD_CALIBRATION_LEN];
extern const uint8_t CMD_HISTORY_START[CMD_HISTORY_START_LEN];
extern const uint8_t CMD_HISTORY_STOP[CMD_HISTORY_STOP_LEN];

// ============================================================================
// Shared State Variables (defined in inkbird_ble.c)
// ============================================================================

// Sensor readings and settings storage
extern inkbird_reading_t s_readings[];
extern inkbird_co2_thresholds_t s_thresholds[];
extern inkbird_co2_settings_t s_co2_settings[];
extern inkbird_alarm_settings_t s_alarm_settings[];
extern inkbird_calibration_t s_calibration[];

// Settings request mode
extern bool s_settings_request_mode;
extern uint8_t s_settings_responses_received;

// Failure tracking
extern uint8_t s_failure_count[];
extern uint8_t s_skip_cycles[];

// Runtime sensor registry
extern inkbird_sensor_config_t s_active_sensors[];
extern uint8_t s_active_sensor_count;

// Discovered sensors
extern inkbird_discovered_t s_discovered[];
extern uint8_t s_discovered_count;

// BLE state
extern bool s_ble_initialized;
extern bool s_running;
extern bool s_scanning;
extern bool s_connected;              // Legacy: any connection active
extern uint16_t s_conn_id;            // Legacy: for single-connection compat
extern uint8_t s_current_sensor_index;
extern esp_gatt_if_t s_gattc_if;

// Peer management (multi-connection)
extern inkbird_peer_t s_peers[MAX_PEERS];
extern uint8_t s_peer_count;

// GATT handles (legacy - kept for history/settings compatibility)
extern uint16_t s_service_start_handle;
extern uint16_t s_service_end_handle;
extern uint16_t s_data_char_handle;
extern uint16_t s_cmd_char_handle;
extern uint16_t s_cccd_handle;

// Synchronization
extern SemaphoreHandle_t s_ble_mutex;
extern SemaphoreHandle_t s_read_complete_sem;

// Task handle
extern TaskHandle_t s_read_task_handle;

// Receive buffer
extern uint8_t s_recv_data[];
extern size_t s_recv_len;
extern bool s_data_received;

// Target address
extern esp_bd_addr_t s_target_bda;

// History download state
extern inkbird_history_state_t s_history_state;
extern inkbird_history_record_t *s_history_records;
extern uint16_t s_history_max_records;
extern uint16_t s_history_expected_count;
extern uint16_t s_history_received_count;
extern uint16_t s_history_stored_count;
extern bool s_history_got_count;
extern uint8_t s_history_buffer[];
extern size_t s_history_buffer_len;
extern SemaphoreHandle_t s_history_complete_sem;
extern uint16_t s_history_write_idx;
extern bool s_history_buffer_wrapped;

// ============================================================================
// Internal Function Declarations
// ============================================================================

// Peer management (inkbird_ble.c)
inkbird_peer_t *peer_find_by_conn_id(uint16_t conn_id);
inkbird_peer_t *peer_find_by_mac(const esp_bd_addr_t bda);
inkbird_peer_t *peer_add(uint8_t sensor_idx);
void peer_remove(inkbird_peer_t *peer);
void peer_reset(inkbird_peer_t *peer);
uint8_t peer_count_connected(void);

// Protocol handlers (inkbird_ble_protocol.c)
void inkbird_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
void inkbird_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

// Data parsing (inkbird_ble_data.c)
bool inkbird_parse_data(const uint8_t *data, size_t len, uint8_t sensor_idx);
void inkbird_read_task(void *arg);

// History (inkbird_ble_history.c)
void inkbird_parse_history_notification(const uint8_t *data, size_t len);
esp_err_t inkbird_send_history_command(const uint8_t *cmd, size_t len);
bool inkbird_is_empty_record(const uint8_t *data);
bool inkbird_parse_history_record(const uint8_t *data);

// Settings (inkbird_ble_settings.c)
uint8_t inkbird_calc_checksum(const uint8_t *data, size_t len);
esp_err_t inkbird_send_settings_command(uint8_t sensor_idx, const uint8_t *cmd, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // INKBIRD_BLE_INTERNAL_H

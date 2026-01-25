/**
 * @file inkbird_ble_internal.h
 * @brief Internal shared state and declarations for Inkbird BLE modules (NimBLE)
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
#include "esp_timer.h"

// NimBLE includes
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// NimBLE store config (provided by ESP-IDF)
void ble_store_config_init(void);

#include "inkbird_ble.h"
#include "inkbird_config.h"
#include "sensor_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define INVALID_HANDLE          0
#define BLE_TASK_CORE           1
#define MAX_PEERS               INKBIRD_SENSOR_COUNT
#define INVALID_CONN_HANDLE     0xFFFF

// ============================================================================
// Per-Connection State (Peer)
// ============================================================================

typedef struct {
    uint8_t sensor_idx;               // Index into s_active_sensors[]
    ble_addr_t remote_addr;           // Device BLE address
    uint16_t conn_handle;             // Connection handle from stack
    bool connected;                   // Connection established
    bool ready;                       // Notifications enabled, ready for data

    // GATT handles (discovered per-connection)
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t data_char_handle;        // FFE4 - notifications
    uint16_t data_char_val_handle;    // FFE4 value handle
    uint16_t cmd_char_handle;         // FFE9 - commands
    uint16_t cmd_char_val_handle;     // FFE9 value handle
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
#define BLE_GATT_DSC_CLT_CFG_UUID16 0x2902

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
extern bool s_ble_synced;
extern bool s_running;
extern bool s_scanning;
extern bool s_connected;              // Legacy: any connection active
extern uint16_t s_conn_handle;        // Legacy: for single-connection compat
extern uint8_t s_current_sensor_index;
extern uint8_t s_own_addr_type;

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
extern ble_addr_t s_target_addr;

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
extern bool s_history_setup_mode;  // True during connection setup for history download
extern uint8_t s_history_downsample_rate;  // Sample every Nth record (1=no downsampling)
extern uint16_t s_history_downsample_counter;  // Counter for downsampling

// ============================================================================
// Internal Function Declarations
// ============================================================================

// Peer management (inkbird_ble.c)
inkbird_peer_t *peer_find_by_conn_handle(uint16_t conn_handle);
inkbird_peer_t *peer_find_by_addr(const ble_addr_t *addr);
inkbird_peer_t *peer_add(uint8_t sensor_idx);
void peer_remove(inkbird_peer_t *peer);
void peer_reset(inkbird_peer_t *peer);
uint8_t peer_count_connected(void);

// NimBLE host task
void inkbird_nimble_host_task(void *param);

// GAP event handler (inkbird_ble_protocol.c)
int inkbird_gap_event_handler(struct ble_gap_event *event, void *arg);

// GATT callbacks (inkbird_ble_protocol.c)
int inkbird_on_disc_complete(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg);
int inkbird_on_chr_disc(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr,
                         void *arg);
int inkbird_on_dsc_disc(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc,
                         void *arg);
int inkbird_on_subscribe(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr,
                          void *arg);
int inkbird_on_write(uint16_t conn_handle,
                      const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr,
                      void *arg);

// Connection management (inkbird_ble_protocol.c)
void inkbird_start_connect(inkbird_peer_t *peer);
void inkbird_start_scan(void);
void inkbird_stop_scan(void);

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

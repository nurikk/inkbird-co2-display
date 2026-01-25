/**
 * @file inkbird_config.h
 * @brief Configuration for Inkbird IAM-T1 CO2 sensors
 *
 * This file contains the MAC address to name mappings for your Inkbird sensors.
 * After running discovery mode, update the MAC addresses below with your actual
 * sensor addresses.
 */

#ifndef INKBIRD_CONFIG_H
#define INKBIRD_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of Inkbird sensors to track
#define INKBIRD_SENSOR_COUNT    4

// Maximum discovered sensors during scan
#define INKBIRD_MAX_DISCOVERED  16

/**
 * @brief Sensor configuration entry
 */
typedef struct {
    uint8_t mac[6];     // BLE MAC address
    char name[16];      // Human-readable display name
    bool enabled;       // Whether this sensor slot is active
} inkbird_sensor_config_t;

/**
 * @brief Sensor MAC address and name mappings
 *
 * Instructions:
 * 1. First boot: Run with all sensors disabled (enabled = false)
 * 2. Check serial output for discovered Inkbird sensors and their MACs
 * 3. Update the MAC addresses in inkbird_ble.c with your sensor addresses
 * 4. Set enabled = true for configured sensors
 * 5. Rebuild and flash
 *
 * MAC format: {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
 * The MAC is printed in the log as AA:BB:CC:DD:EE:FF
 *
 * Defined in inkbird_ble.c
 */
extern const inkbird_sensor_config_t INKBIRD_SENSORS[INKBIRD_SENSOR_COUNT];

// ============================================================================
// BLE Protocol Configuration (Inkbird IAM-T1)
// ============================================================================

// Service UUID for Inkbird IAM-T1 sensors
#define INKBIRD_SERVICE_UUID            0xFFE0

// Characteristic UUID for sensor data notifications
#define INKBIRD_CHAR_DATA_UUID          0xFFE4

// Characteristic UUID for write commands (to trigger data output)
#define INKBIRD_CHAR_CMD_UUID           0xFFE9

// ============================================================================
// Timing Configuration
// ============================================================================

// BLE scan duration in seconds (for discovery mode)
#define INKBIRD_SCAN_DURATION_SEC       30

// Connection timeout in milliseconds (wait for connection + notifications)
// IAM-T1 sensor sends data every ~1-2 minutes after connection
#define INKBIRD_CONNECT_TIMEOUT_MS      180000

// Interval between sensor read cycles (milliseconds)
// Each cycle reads all enabled sensors sequentially
#define INKBIRD_READ_INTERVAL_MS        60000

// Time after which data is considered stale (milliseconds)
// If no new data received within this time, reading.stale = true
#define INKBIRD_DATA_STALE_MS           180000

// Delay between reading different sensors (milliseconds)
#define INKBIRD_INTER_SENSOR_DELAY_MS   2000

// ============================================================================
// Retry Configuration
// ============================================================================

// Number of consecutive failures before skipping a sensor
#define INKBIRD_MAX_FAILURES            3

// Number of read cycles to skip after max failures reached
#define INKBIRD_SKIP_CYCLES_ON_FAILURE  10

// ============================================================================
// Startup Configuration
// ============================================================================

// Timeout for initial real-time sensor read at startup (milliseconds)
// Shorter than normal timeout for snappier startup
#define INKBIRD_STARTUP_READ_TIMEOUT_MS  30000

// Number of retry attempts for initial sensor read at startup
#define INKBIRD_STARTUP_READ_RETRIES     3

// Timeout for connection test per sensor (milliseconds)
// Used during smart discovery to quickly check if known sensors are reachable
#define INKBIRD_CONNECTION_TEST_TIMEOUT_MS  10000

// ============================================================================
// History Download Configuration
// ============================================================================

// Timeout for history download connection setup (milliseconds)
#define INKBIRD_HISTORY_CONNECT_TIMEOUT_MS  30000

// Timeout for complete history download (milliseconds) - 5 minutes
// Large datasets with 1500+ records can take several minutes to transfer
#define INKBIRD_HISTORY_DOWNLOAD_TIMEOUT_MS 300000

// Maximum downsample rate for history - never skip more than this many records
// Ensures we always get meaningful data coverage even with early termination
#define INKBIRD_MAX_DOWNSAMPLE_RATE  50

// ============================================================================
// NVS Configuration
// ============================================================================

// NVS namespace for storing discovered sensors
#define INKBIRD_NVS_NAMESPACE           "inkbird"

// Maximum sensors to store in NVS (separate from compile-time config)
#define INKBIRD_NVS_MAX_SENSORS         8

#ifdef __cplusplus
}
#endif

#endif // INKBIRD_CONFIG_H

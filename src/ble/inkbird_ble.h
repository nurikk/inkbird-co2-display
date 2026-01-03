/**
 * @file inkbird_ble.h
 * @brief BLE reader for Inkbird IAM-T1 CO2 sensors
 *
 * Provides functionality to discover, connect to, and read data from
 * Inkbird IAM-T1 CO2 sensors via Bluetooth Low Energy.
 *
 * Usage:
 * 1. Call inkbird_ble_init() once at startup
 * 2. Call inkbird_ble_discover() to find sensors (check serial logs)
 * 3. Update inkbird_config.h with discovered MAC addresses
 * 4. Call inkbird_ble_start() to begin periodic sensor reading
 * 5. Call inkbird_ble_get_reading() to get latest sensor data
 */

#ifndef INKBIRD_BLE_H
#define INKBIRD_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "inkbird_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sensor reading from Inkbird IAM-T1
 *
 * All values are in fixed-point format for efficiency:
 * - temperature: 0.1°C units (e.g., 234 = 23.4°C)
 * - humidity: 0.1% units (e.g., 567 = 56.7%)
 */
typedef struct {
    uint16_t co2_ppm;       // CO2 concentration in ppm (0-5000 typical)
    int16_t  temperature;   // Temperature in 0.1°C units
    uint16_t humidity;      // Relative humidity in 0.1% units
    uint16_t pressure;      // Atmospheric pressure in hPa
    uint32_t timestamp;     // Reading timestamp (ms since boot)
    bool     valid;         // true if data has been received at least once
    bool     stale;         // true if data is older than INKBIRD_DATA_STALE_MS
} inkbird_reading_t;

/**
 * @brief Discovered sensor information (from BLE scan)
 */
typedef struct {
    uint8_t mac[6];         // BLE MAC address
    char    name[32];       // Device advertised name (if any)
    int8_t  rssi;           // Signal strength (dBm)
    uint8_t addr_type;      // BLE address type (0=public, 1=random)
} inkbird_discovered_t;

/**
 * @brief Initialize BLE subsystem for Inkbird sensors
 *
 * Initializes the NimBLE stack and prepares for scanning/connecting.
 * Must be called once at application startup before other functions.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_init(void);

/**
 * @brief Start BLE reading operations
 *
 * Begins the periodic connect-read-disconnect cycle for all enabled
 * sensors configured in inkbird_config.h. Readings are performed
 * in round-robin order.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_start(void);

/**
 * @brief Stop BLE reading operations
 *
 * Stops the periodic reading task and disconnects from any
 * currently connected sensors.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_stop(void);

/**
 * @brief Get the latest reading for a sensor
 *
 * Returns the most recent reading received from the specified sensor.
 * Check the 'valid' flag to see if any data has been received.
 * Check the 'stale' flag to see if the data is recent.
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return Sensor reading structure
 */
inkbird_reading_t inkbird_ble_get_reading(uint8_t index);

/**
 * @brief Check if a sensor is currently connected
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return true if sensor is connected, false otherwise
 */
bool inkbird_ble_is_connected(uint8_t index);

/**
 * @brief Start discovery scan for Inkbird sensors
 *
 * Scans for BLE devices advertising the Inkbird service UUID.
 * Discovered sensors are logged to the serial console with their
 * MAC addresses. Use this to find the MAC addresses of your sensors,
 * then update inkbird_config.h.
 *
 * This is a blocking call that takes INKBIRD_SCAN_DURATION_SEC seconds.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_discover(void);

/**
 * @brief Get number of sensors discovered in last scan
 *
 * @return Number of discovered Inkbird sensors
 */
uint8_t inkbird_ble_get_discovered_count(void);

/**
 * @brief Get information about a discovered sensor
 *
 * @param index Discovery index (0 to discovered_count-1)
 * @param out_info Pointer to store discovered sensor info
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t inkbird_ble_get_discovered(uint8_t index, inkbird_discovered_t *out_info);

/**
 * @brief Get sensor name from configuration
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return Pointer to sensor name string, or "Unknown" if invalid index
 */
const char *inkbird_ble_get_sensor_name(uint8_t index);

/**
 * @brief Check if a sensor slot is enabled in configuration
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return true if sensor is enabled, false otherwise
 */
bool inkbird_ble_is_sensor_enabled(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif // INKBIRD_BLE_H

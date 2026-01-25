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
    int64_t  timestamp;     // Reading timestamp (ms since boot, from esp_timer)
    bool     valid;         // true if data has been received at least once
    bool     stale;         // true if data is older than INKBIRD_DATA_STALE_MS
} inkbird_reading_t;

/**
 * @brief Historical data record from sensor memory
 *
 * The sensor stores readings at configurable intervals (typically 10 minutes).
 * Each record contains environmental data at that point in time.
 */
typedef struct {
    uint16_t co2_ppm;       // CO2 concentration in ppm
    int16_t  temperature;   // Temperature in 0.1°C units
    uint16_t humidity;      // Relative humidity in 0.1% units
    uint16_t pressure;      // Atmospheric pressure in hPa
    uint8_t  interval_mins; // Recording interval in minutes
    bool     is_fahrenheit; // true if sensor was set to Fahrenheit
} inkbird_history_record_t;

/**
 * @brief History download state
 */
typedef enum {
    INKBIRD_HISTORY_IDLE = 0,       // Not downloading
    INKBIRD_HISTORY_REQUESTING,     // Command sent, waiting for count
    INKBIRD_HISTORY_RECEIVING,      // Receiving data records
    INKBIRD_HISTORY_COMPLETE,       // Download finished successfully
    INKBIRD_HISTORY_ERROR,          // Download failed
} inkbird_history_state_t;

/**
 * @brief History download result
 */
typedef struct {
    inkbird_history_state_t state;
    uint16_t expected_count;        // Number of records expected
    uint16_t received_count;        // Number of records received
    inkbird_history_record_t *records;  // Pointer to record array (caller-allocated)
    uint16_t max_records;           // Size of records array
} inkbird_history_result_t;

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
 * @brief CO2 threshold settings synced from sensor
 *
 * The sensor supports two threshold modes:
 * - Normal mode: Fixed default thresholds (420-2000 PPM)
 * - Plant/Custom mode: User-configurable thresholds
 *
 * The 'use_custom' flag indicates which mode the sensor is configured for.
 */
typedef struct {
    uint16_t normal_low_ppm;   // Normal mode low threshold (default: 420)
    uint16_t normal_high_ppm;  // Normal mode high threshold (default: 2000)
    uint16_t plant_low_ppm;    // Plant/custom mode low threshold
    uint16_t plant_high_ppm;   // Plant/custom mode high threshold
    bool     use_custom;       // true = use plant/custom thresholds, false = use normal
    bool     settings_valid;   // true if CO2 settings (0x02) have been synced
    bool     thresholds_valid; // true if thresholds (0x03) have been synced
} inkbird_co2_thresholds_t;

/**
 * @brief CO2 calibration settings (Command 0x02)
 */
typedef struct {
    uint8_t  display_mode;     // Display mode (0-4)
    bool     use_custom;       // Custom/plant mode enabled
    bool     auto_calibration; // Auto calibration enabled
    uint8_t  manual_mode;      // 0=off, 1=calibrating, 4=reset CO2
    uint16_t manual_cal_value; // Manual calibration value (PPM)
    bool     valid;            // Data has been synced
} inkbird_co2_settings_t;

/**
 * @brief CO2 alarm settings (Command 0x04)
 */
typedef struct {
    bool     enabled;          // Alarm enabled
    uint8_t  alarm_mode;       // Alarm behavior mode
    uint16_t alarm_value;      // CO2 alarm threshold (PPM)
    bool     valid;            // Data has been synced
} inkbird_alarm_settings_t;

/**
 * @brief Calibration offset settings (Command 0x05)
 */
typedef struct {
    int16_t  co2_offset;       // CO2 offset in PPM (signed)
    int16_t  temp_offset;      // Temperature offset * 10 (signed)
    int16_t  hum_offset;       // Humidity offset * 10 (signed)
    bool     use_fahrenheit;   // Temperature unit (false=Celsius, true=Fahrenheit)
    bool     valid;            // Data has been synced
} inkbird_calibration_t;

/**
 * @brief Combined device settings
 */
typedef struct {
    inkbird_co2_settings_t   co2_settings;   // CO2 mode/calibration (0x02)
    inkbird_co2_thresholds_t thresholds;     // CO2 thresholds (0x03)
    inkbird_alarm_settings_t alarm;          // Alarm settings (0x04)
    inkbird_calibration_t    calibration;    // Calibration offsets (0x05)
} inkbird_device_settings_t;

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
 * @brief Read current sensor value (one-shot blocking call)
 *
 * Connects to the specified sensor, waits for a single real-time
 * notification, parses the data, and disconnects. Used for quick
 * startup reads before beginning periodic polling.
 *
 * @param sensor_idx Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @param timeout_ms Connection/read timeout in milliseconds
 * @param out_reading Pointer to store the reading (can be NULL)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout, error code otherwise
 */
esp_err_t inkbird_ble_read_sensor_once(uint8_t sensor_idx,
                                        uint32_t timeout_ms,
                                        inkbird_reading_t *out_reading);

/**
 * @brief Check if a sensor is currently connected
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return true if sensor is connected, false otherwise
 */
bool inkbird_ble_is_connected(uint8_t index);

/**
 * @brief Request historical data download from sensor
 *
 * Initiates a history download from the specified sensor.
 * This is a blocking call that connects, downloads history, then disconnects.
 *
 * @param sensor_idx Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @param records Pre-allocated array to store history records
 * @param max_records Maximum number of records the array can hold
 * @param out_count Pointer to store actual number of records received
 * @return ESP_OK if download succeeded, error code otherwise
 */
esp_err_t inkbird_ble_download_history(uint8_t sensor_idx,
                                        inkbird_history_record_t *records,
                                        uint16_t max_records,
                                        uint16_t *out_count);

/**
 * @brief Cancel ongoing history download
 *
 * Sends cancel command to sensor and stops history download.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_cancel_history(void);

/**
 * @brief Get current history download state
 *
 * @return Current history download state
 */
inkbird_history_state_t inkbird_ble_get_history_state(void);

/**
 * @brief Get current history download progress
 *
 * @param out_expected Pointer to store expected record count
 * @param out_received Pointer to store received record count
 */
void inkbird_ble_get_history_progress(uint16_t *out_expected, uint16_t *out_received);

/**
 * @brief Get the downsample rate used in the last history download
 *
 * When the sensor has more records than the buffer can hold, records are
 * downsampled by storing every Nth record. This function returns that N value.
 * Use this to correctly reconstruct timestamps when processing downloaded data.
 *
 * @return Downsample rate (1 = no downsampling, N = every Nth record stored)
 */
uint8_t inkbird_ble_get_history_downsample_rate(void);

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
 * @brief Register discovered sensors to active sensor list
 *
 * After running inkbird_ble_discover(), call this function to add
 * newly discovered sensors to the active sensor list. Configured
 * sensors (from inkbird_config.h) have priority and are loaded first.
 * Discovered sensors that aren't already configured will be added
 * to any remaining slots.
 */
void inkbird_ble_register_discovered(void);

/**
 * @brief Get number of active sensors
 *
 * Returns the count of sensors currently active (configured + auto-discovered).
 *
 * @return Number of active sensors (0 to INKBIRD_SENSOR_COUNT)
 */
uint8_t inkbird_ble_get_active_count(void);

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

/**
 * @brief Get CO2 thresholds synced from sensor
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return CO2 threshold structure (check valid flag)
 */
inkbird_co2_thresholds_t inkbird_ble_get_thresholds(uint8_t index);

/**
 * @brief Get all device settings for a sensor
 *
 * @param index Sensor index (0 to INKBIRD_SENSOR_COUNT-1)
 * @return Combined device settings structure
 */
inkbird_device_settings_t inkbird_ble_get_settings(uint8_t index);

/**
 * @brief Request settings sync from sensor
 *
 * Sends commands to query CO2 settings, thresholds, alarm, and calibration.
 * Results will be available via inkbird_ble_get_settings() after responses arrive.
 *
 * @param sensor_idx Sensor index
 * @param timeout_ms Timeout for connection and data
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_request_settings(uint8_t sensor_idx, uint32_t timeout_ms);

/**
 * @brief Set CO2 thresholds on device
 *
 * @param sensor_idx Sensor index
 * @param normal_high Normal mode high threshold (PPM)
 * @param normal_low Normal mode low threshold (PPM)
 * @param plant_high Plant mode high threshold (PPM)
 * @param plant_low Plant mode low threshold (PPM)
 * @param reset_to_defaults If true, reset thresholds to factory defaults
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_set_thresholds(uint8_t sensor_idx,
                                      uint16_t normal_high, uint16_t normal_low,
                                      uint16_t plant_high, uint16_t plant_low,
                                      bool reset_to_defaults);

/**
 * @brief Set CO2 alarm settings on device
 *
 * @param sensor_idx Sensor index
 * @param enabled Alarm enabled
 * @param alarm_mode Alarm behavior mode
 * @param alarm_value CO2 threshold for alarm (PPM)
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_set_alarm(uint8_t sensor_idx,
                                 bool enabled, uint8_t alarm_mode, uint16_t alarm_value);

/**
 * @brief Set calibration offsets on device
 *
 * @param sensor_idx Sensor index
 * @param co2_offset CO2 offset in PPM (signed)
 * @param temp_offset Temperature offset * 10 (signed)
 * @param hum_offset Humidity offset * 10 (signed)
 * @param use_fahrenheit Temperature unit
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_set_calibration(uint8_t sensor_idx,
                                       int16_t co2_offset, int16_t temp_offset,
                                       int16_t hum_offset, bool use_fahrenheit);

/**
 * @brief Set CO2 mode settings on device
 *
 * @param sensor_idx Sensor index
 * @param display_mode Display mode (0-4)
 * @param use_custom Use custom/plant mode
 * @param auto_calibration Enable auto calibration
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_set_co2_mode(uint8_t sensor_idx,
                                    uint8_t display_mode, bool use_custom,
                                    bool auto_calibration);

/**
 * @brief Trigger manual CO2 calibration
 *
 * @param sensor_idx Sensor index
 * @param cal_value Calibration reference value (PPM), typically 400-450 for outdoor air
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_calibrate_co2(uint8_t sensor_idx, uint16_t cal_value);

/**
 * @brief Reset CO2 sensor
 *
 * @param sensor_idx Sensor index
 * @return ESP_OK on success
 */
esp_err_t inkbird_ble_reset_co2(uint8_t sensor_idx);

// ============================================================================
// Smart Discovery APIs (skip discovery if known sensors are reachable)
// ============================================================================

/**
 * @brief Connection test result
 */
typedef struct {
    uint8_t reachable_count;    // Number of sensors that responded
    uint8_t total_count;        // Total sensors tested
    bool all_reachable;         // true if all known sensors responded
} inkbird_connection_test_result_t;

/**
 * @brief Load known sensors from config and NVS
 *
 * Merges sensors from INKBIRD_SENSORS (compile-time config) with
 * sensors stored in NVS (runtime discovered). Config sensors take
 * priority for naming if MAC addresses match.
 *
 * Call this instead of relying on inkbird_ble_init() to load sensors.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_load_known_sensors(void);

/**
 * @brief Test connectivity to all known sensors
 *
 * Attempts a quick connection to each active sensor with a short timeout.
 * Use this to check if known sensors are reachable before deciding
 * whether to run full discovery.
 *
 * @param result Pointer to store test results
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_test_connections(inkbird_connection_test_result_t *result);

/**
 * @brief Save discovered sensors to NVS
 *
 * Persists any sensors in the active list that aren't already in
 * INKBIRD_SENSORS config. Call after discovery to remember new sensors
 * across reboots.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_save_to_nvs(void);

/**
 * @brief Clear all sensors stored in NVS
 *
 * Removes all persisted sensor data from NVS. Config sensors
 * (INKBIRD_SENSORS) are not affected.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t inkbird_ble_clear_nvs(void);

#ifdef __cplusplus
}
#endif

#endif // INKBIRD_BLE_H

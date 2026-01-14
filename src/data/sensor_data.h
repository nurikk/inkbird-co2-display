/**
 * @file sensor_data.h
 * @brief Sensor data structures and history management
 *
 * Manages CO2, temperature, and humidity data for 4 sensors,
 * including historical data stored in ring buffers.
 */

#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of sensors
#define SENSOR_COUNT        4

// History buffer size (60 samples = ~1 hour at 1 sample/min)
#define SENSOR_HISTORY_SIZE 60

// CO2 level thresholds (in ppm)
#define CO2_LEVEL_GOOD      800
#define CO2_LEVEL_MODERATE  1000
#define CO2_LEVEL_WARNING   1400

/**
 * @brief CO2 air quality level
 */
typedef enum {
    CO2_STATUS_GOOD,      // < 800 ppm - excellent air quality
    CO2_STATUS_MODERATE,  // 800-1000 ppm - acceptable
    CO2_STATUS_WARNING,   // 1000-1400 ppm - ventilation recommended
    CO2_STATUS_ALERT      // > 1400 ppm - poor air quality, ventilate!
} co2_status_t;

/**
 * @brief Single sensor reading
 */
typedef struct {
    uint16_t co2_ppm;       // CO2 concentration in ppm (0-5000 typical)
    int16_t  temperature;   // Temperature in 0.1°C units (e.g., 234 = 23.4°C)
    uint16_t humidity;      // Relative humidity in 0.1% units (e.g., 456 = 45.6%)
    uint16_t pressure;      // Atmospheric pressure in hPa (e.g., 1013 = 1013 hPa)
    uint32_t timestamp;     // Reading timestamp (milliseconds since boot)
} sensor_reading_t;

/**
 * @brief Sensor data with history
 */
typedef struct {
    uint8_t id;                                 // Sensor ID (0-3)
    char name[16];                              // Sensor name (e.g., "Sensor 1")
    sensor_reading_t current;                   // Most recent reading
    int16_t co2_history[SENSOR_HISTORY_SIZE];   // CO2 history ring buffer
    uint8_t history_head;                       // Ring buffer head index
    uint8_t history_count;                      // Number of valid history entries
    bool connected;                             // Sensor connection status
    bool downloading;                           // True while downloading history
    uint16_t download_expected;                 // Expected record count for download
    uint16_t download_received;                 // Received record count for download
    char status_text[24];                       // Current status (e.g., "Connecting...", "Reading...")
} sensor_data_t;

/**
 * @brief Initialize sensor data module
 */
void sensor_data_init(void);

/**
 * @brief Get sensor data by index
 *
 * @param index Sensor index (0-3)
 * @return Pointer to sensor data, or NULL if index invalid
 */
sensor_data_t *sensor_data_get(uint8_t index);

/**
 * @brief Update sensor reading
 *
 * Adds the reading to the sensor and updates history.
 *
 * @param index Sensor index (0-3)
 * @param reading New sensor reading
 */
void sensor_data_update(uint8_t index, const sensor_reading_t *reading);

/**
 * @brief Add CO2 value to history only
 *
 * Adds a CO2 reading to the history buffer without updating the current reading.
 * Use this for loading historical data from sensor memory.
 *
 * @param index Sensor index (0-3)
 * @param co2_ppm CO2 value in ppm
 */
void sensor_data_add_history(uint8_t index, uint16_t co2_ppm);

/**
 * @brief Get CO2 status level
 *
 * @param co2_ppm CO2 concentration in ppm
 * @return CO2 status level
 */
co2_status_t sensor_data_get_co2_status(uint16_t co2_ppm);

/**
 * @brief Get status text for CO2 level
 *
 * @param status CO2 status level
 * @return Status string (e.g., "[OK]", "[!!!]")
 */
const char *sensor_data_get_status_text(co2_status_t status);

/**
 * @brief Get CO2 history as array for charting
 *
 * Returns a pointer to the CO2 history array with values in chronological order.
 * The returned array has SENSOR_HISTORY_SIZE elements.
 *
 * @param index Sensor index (0-3)
 * @param out_count Output: number of valid data points
 * @return Pointer to history array (internal buffer, do not free)
 */
const int16_t *sensor_data_get_co2_history(uint8_t index, uint8_t *out_count);

/**
 * @brief Set sensor name
 *
 * @param index Sensor index (0-3)
 * @param name New sensor name (max 15 chars)
 */
void sensor_data_set_name(uint8_t index, const char *name);

/**
 * @brief Set sensor status text
 *
 * @param index Sensor index (0-3)
 * @param status Status text to display (max 23 chars)
 */
void sensor_data_set_status(uint8_t index, const char *status);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_DATA_H

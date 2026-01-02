/**
 * @file synthetic_data.h
 * @brief Synthetic sensor data generator for demo/testing
 *
 * Generates realistic CO2, temperature, and humidity data
 * with various patterns for each sensor.
 */

#ifndef SYNTHETIC_DATA_H
#define SYNTHETIC_DATA_H

#include <stdint.h>
#include "sensor_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize synthetic data generator
 *
 * Seeds random number generator and sets initial sensor values.
 */
void synthetic_data_init(void);

/**
 * @brief Generate next set of readings for all sensors
 *
 * Updates all 4 sensors with new synthetic readings.
 * Call this periodically (e.g., every minute) to simulate real sensors.
 */
void synthetic_data_update(void);

/**
 * @brief Pre-fill history with synthetic data
 *
 * Fills the history buffers with realistic data so charts
 * show something interesting from the start.
 *
 * @param points Number of history points to generate (max SENSOR_HISTORY_SIZE)
 */
void synthetic_data_prefill_history(uint8_t points);

/**
 * @brief Get current simulation time step
 *
 * @return Current time step counter
 */
uint32_t synthetic_data_get_step(void);

#ifdef __cplusplus
}
#endif

#endif // SYNTHETIC_DATA_H

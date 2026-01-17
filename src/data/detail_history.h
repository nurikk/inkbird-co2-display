/**
 * @file detail_history.h
 * @brief Extended history buffer for detail screen
 *
 * Manages a large shared buffer (2880 points) for displaying extended
 * history data on the sensor detail screen. Data is downloaded on-demand
 * when the user taps a sensor tile to view details.
 *
 * Architecture:
 * - Main screen uses short per-sensor buffers (60 points in sensor_data_t)
 * - Detail screen uses this shared large buffer (2880 points)
 * - Data is downloaded via BLE when detail screen is opened
 */

#ifndef DETAIL_HISTORY_H
#define DETAIL_HISTORY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Display dimensions (must match ui_co2_display.c)
#define DETAIL_DISPLAY_WIDTH       480
#define DETAIL_CHART_MARGIN_X      12
#define DETAIL_PLOT_MARGIN_LEFT    32
#define DETAIL_PLOT_MARGIN_RIGHT   28

// Calculate plot width in pixels: 480 - 2*12 - 32 - 28 = 396 pixels
#define DETAIL_PLOT_WIDTH  (DETAIL_DISPLAY_WIDTH - (DETAIL_CHART_MARGIN_X * 2) \
                            - DETAIL_PLOT_MARGIN_LEFT - DETAIL_PLOT_MARGIN_RIGHT)

// Memory constraint: BLE stack needs ~8KB free heap during operations.
// Each record: 10 bytes (inkbird_history_record_t) + 10 bytes (5 int16_t arrays) = 20 bytes
// Using 170 records = 3.4KB total, leaving headroom for BLE callbacks.
// Chart rendering will scale 170 data points across 396 pixels.
#define DETAIL_HISTORY_MAX_RECORDS  170
#define DETAIL_HISTORY_SIZE  DETAIL_HISTORY_MAX_RECORDS

/**
 * @brief Download state for detail history
 */
typedef enum {
    DETAIL_HISTORY_IDLE = 0,       // No download in progress
    DETAIL_HISTORY_IN_PROGRESS,    // Download active
    DETAIL_HISTORY_COMPLETE,       // Download finished successfully
    DETAIL_HISTORY_CANCELLED,      // Download was cancelled by user
    DETAIL_HISTORY_ERROR           // Download failed
} detail_history_state_t;

/**
 * @brief Initialize detail history module
 *
 * Allocates internal buffers. Call once at startup.
 */
void detail_history_init(void);

/**
 * @brief Start async download of extended history for a sensor
 *
 * Begins downloading history data in a background task.
 * The download runs on the BLE core and does not block.
 *
 * @param sensor_idx Sensor index (0-3)
 * @return true if download started, false if already in progress or invalid index
 */
bool detail_history_start_download(uint8_t sensor_idx);

/**
 * @brief Cancel ongoing download
 *
 * Stops the download and retains any data received so far.
 * Safe to call even if no download is in progress.
 */
void detail_history_cancel_download(void);

/**
 * @brief Get current download state
 *
 * @return Current download state
 */
detail_history_state_t detail_history_get_state(void);

/**
 * @brief Get download progress as percentage (0-100)
 *
 * @return Progress percentage, or 0 if not downloading
 */
uint8_t detail_history_get_progress(void);

/**
 * @brief Get number of valid data points in the buffer
 *
 * @return Number of valid history entries
 */
uint16_t detail_history_get_count(void);

/**
 * @brief Get sensor index currently being downloaded or last downloaded
 *
 * @return Sensor index (0-3), or 0xFF if none
 */
uint8_t detail_history_get_sensor_idx(void);

/**
 * @brief Get CO2 history array
 *
 * Returns pointer to internal buffer. Data is in chronological order
 * (oldest first, newest last).
 *
 * @param out_count Output: number of valid entries
 * @return Pointer to CO2 history array (internal buffer, do not free)
 */
const int16_t *detail_history_get_co2(uint16_t *out_count);

/**
 * @brief Get temperature history array (0.1 C units)
 */
const int16_t *detail_history_get_temp(uint16_t *out_count);

/**
 * @brief Get humidity history array (0.1% units)
 */
const int16_t *detail_history_get_hum(uint16_t *out_count);

/**
 * @brief Get pressure history array (hPa)
 */
const int16_t *detail_history_get_pres(uint16_t *out_count);

/**
 * @brief Get time offsets array (minutes ago from now)
 *
 * Index 0 is oldest (most minutes ago), index N-1 is newest (0 = now).
 */
const uint16_t *detail_history_get_time_offsets(uint16_t *out_count);

/**
 * @brief Get total time span of history in minutes
 *
 * @return Total minutes from oldest to newest record, 0 if no history
 */
uint16_t detail_history_get_total_minutes(void);

/**
 * @brief Clear the detail history buffer
 *
 * Resets all data and state. Called when returning to main screen.
 */
void detail_history_clear(void);

#ifdef __cplusplus
}
#endif

#endif // DETAIL_HISTORY_H

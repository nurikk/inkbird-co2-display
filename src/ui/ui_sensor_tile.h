/**
 * @file ui_sensor_tile.h
 * @brief Sensor tile UI component
 *
 * Reusable widget displaying CO2, temperature, humidity,
 * and a historical chart for a single sensor.
 */

#ifndef UI_SENSOR_TILE_H
#define UI_SENSOR_TILE_H

#include "lvgl.h"
#include "sensor_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sensor tile widget handle
 */
typedef struct {
    lv_obj_t *container;      // Main tile container
    lv_obj_t *title_label;    // Sensor name
    lv_obj_t *status_label;   // Status indicator ([OK], [!!!], etc.)
    lv_obj_t *co2_label;      // Large CO2 value
    lv_obj_t *temp_label;     // Temperature value
    lv_obj_t *humidity_label; // Humidity value
    lv_obj_t *chart;          // History chart
    lv_chart_series_t *series; // Chart data series
    uint8_t sensor_index;     // Which sensor this tile displays
} ui_sensor_tile_t;

/**
 * @brief Create a sensor tile widget
 *
 * @param parent Parent object to contain the tile
 * @param sensor_index Index of the sensor (0-3)
 * @return Pointer to created tile, or NULL on failure
 */
ui_sensor_tile_t *ui_sensor_tile_create(lv_obj_t *parent, uint8_t sensor_index);

/**
 * @brief Update tile with current sensor data
 *
 * Refreshes all displayed values and chart from sensor data.
 *
 * @param tile Tile to update
 */
void ui_sensor_tile_update(ui_sensor_tile_t *tile);

/**
 * @brief Set tile size
 *
 * @param tile Tile to resize
 * @param width Width in pixels
 * @param height Height in pixels
 */
void ui_sensor_tile_set_size(ui_sensor_tile_t *tile, int32_t width, int32_t height);

#ifdef __cplusplus
}
#endif

#endif // UI_SENSOR_TILE_H

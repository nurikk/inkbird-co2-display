/**
 * @file ui_sensor_tile.c
 * @brief Sensor tile UI component implementation
 *
 * Layout of each tile (approximately 198x148 pixels):
 * ┌────────────────────────────────────┐
 * │ Sensor 1                    [OK]  │  <- Title row
 * │         845 ppm                   │  <- Large CO2 value
 * │    23.4°C      45%               │  <- Temp & Humidity
 * │ ┌──────────────────────────────┐ │
 * │ │ ▁▂▃▄▅▆▅▄▃▂▁▂▃▄▅▆▅▄▃▂▁▂▃▄▅▆ │ │  <- Mini chart
 * │ └──────────────────────────────┘ │
 * └────────────────────────────────────┘
 */

#include <stdio.h>
#include <stdlib.h>
#include "ui_sensor_tile.h"
#include "ui_styles.h"
#include "sensor_data.h"

// Chart configuration
#define CHART_POINT_COUNT   SENSOR_HISTORY_SIZE
#define CHART_HEIGHT        45

ui_sensor_tile_t *ui_sensor_tile_create(lv_obj_t *parent, uint8_t sensor_index)
{
    ui_sensor_tile_t *tile = lv_malloc(sizeof(ui_sensor_tile_t));
    if (tile == NULL) {
        return NULL;
    }
    
    tile->sensor_index = sensor_index;
    
    // Create main container
    tile->container = lv_obj_create(parent);
    lv_obj_add_style(tile->container, ui_style_tile(), 0);
    lv_obj_set_layout(tile->container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tile->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile->container, LV_FLEX_ALIGN_START, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tile->container, LV_OBJ_FLAG_SCROLLABLE);
    
    // --- Row 1: Title and Status ---
    lv_obj_t *title_row = lv_obj_create(tile->container);
    lv_obj_set_size(title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_layout(title_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Sensor name
    tile->title_label = lv_label_create(title_row);
    lv_obj_add_style(tile->title_label, ui_style_title(), 0);
    lv_label_set_text_fmt(tile->title_label, "Sensor %d", sensor_index + 1);
    
    // Status indicator
    tile->status_label = lv_label_create(title_row);
    lv_obj_add_style(tile->status_label, ui_style_status(), 0);
    lv_label_set_text(tile->status_label, "[--]");
    
    // --- Row 2: Large CO2 value ---
    tile->co2_label = lv_label_create(tile->container);
    lv_obj_add_style(tile->co2_label, ui_style_co2_value(), 0);
    lv_label_set_text(tile->co2_label, "--- ppm");
    lv_obj_set_style_pad_top(tile->co2_label, 2, 0);
    
    // --- Row 3: Temperature and Humidity ---
    lv_obj_t *env_row = lv_obj_create(tile->container);
    lv_obj_set_size(env_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(env_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(env_row, 0, 0);
    lv_obj_set_style_pad_all(env_row, 0, 0);
    lv_obj_set_layout(env_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(env_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(env_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(env_row, LV_OBJ_FLAG_SCROLLABLE);
    
    tile->temp_label = lv_label_create(env_row);
    lv_obj_add_style(tile->temp_label, ui_style_secondary(), 0);
    lv_label_set_text(tile->temp_label, "--.-C");
    
    tile->humidity_label = lv_label_create(env_row);
    lv_obj_add_style(tile->humidity_label, ui_style_secondary(), 0);
    lv_label_set_text(tile->humidity_label, "--%");
    
    // --- Row 4: History Chart ---
    tile->chart = lv_chart_create(tile->container);
    lv_obj_set_size(tile->chart, LV_PCT(100), CHART_HEIGHT);
    lv_obj_add_style(tile->chart, ui_style_chart(), 0);
    lv_obj_set_style_pad_top(tile->chart, 4, 0);
    
    // Configure chart
    lv_chart_set_type(tile->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(tile->chart, CHART_POINT_COUNT);
    lv_chart_set_range(tile->chart, LV_CHART_AXIS_PRIMARY_Y, 300, 2000);
    lv_chart_set_div_line_count(tile->chart, 0, 0);  // No grid lines
    lv_chart_set_update_mode(tile->chart, LV_CHART_UPDATE_MODE_SHIFT);
    
    // Hide point markers for cleaner look on e-paper
    lv_obj_set_style_size(tile->chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(tile->chart, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(tile->chart, lv_color_black(), LV_PART_ITEMS);
    
    // Add data series
    tile->series = lv_chart_add_series(tile->chart, lv_color_black(), 
                                        LV_CHART_AXIS_PRIMARY_Y);
    
    // Initialize with zeros
    for (int i = 0; i < CHART_POINT_COUNT; i++) {
        lv_chart_set_next_value(tile->chart, tile->series, 400);
    }
    
    return tile;
}

void ui_sensor_tile_update(ui_sensor_tile_t *tile)
{
    if (tile == NULL) {
        return;
    }
    
    sensor_data_t *sensor = sensor_data_get(tile->sensor_index);
    if (sensor == NULL) {
        return;
    }
    
    // Update sensor name
    lv_label_set_text(tile->title_label, sensor->name);
    
    // Update CO2 value and status
    uint16_t co2 = sensor->current.co2_ppm;
    co2_status_t status = sensor_data_get_co2_status(co2);
    const char *status_text = sensor_data_get_status_text(status);
    
    lv_label_set_text_fmt(tile->co2_label, "%d ppm", co2);
    lv_label_set_text(tile->status_label, status_text);
    
    // Update temperature (convert from 0.1°C to display format)
    int16_t temp = sensor->current.temperature;
    int temp_whole = temp / 10;
    int temp_frac = abs(temp % 10);
    lv_label_set_text_fmt(tile->temp_label, "%d.%dC", temp_whole, temp_frac);
    
    // Update humidity (convert from 0.1% to display format)
    uint16_t humidity = sensor->current.humidity;
    int hum_whole = humidity / 10;
    lv_label_set_text_fmt(tile->humidity_label, "%d%%", hum_whole);
    
    // Update chart with history data
    uint8_t history_count;
    const int16_t *history = sensor_data_get_co2_history(tile->sensor_index, &history_count);
    
    if (history != NULL && history_count > 0) {
        // Get the series y-points array and update directly
        int32_t *y_points = lv_chart_get_y_array(tile->chart, tile->series);
        
        for (int i = 0; i < CHART_POINT_COUNT; i++) {
            if (i < history_count) {
                y_points[i] = history[i];
            } else {
                y_points[i] = history[history_count - 1];  // Repeat last value
            }
        }
        
        lv_chart_refresh(tile->chart);
    }
}

void ui_sensor_tile_set_size(ui_sensor_tile_t *tile, int32_t width, int32_t height)
{
    if (tile == NULL || tile->container == NULL) {
        return;
    }
    
    lv_obj_set_size(tile->container, width, height);
}

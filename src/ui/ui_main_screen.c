/**
 * @file ui_main_screen.c
 * @brief Main dashboard screen with 2x2 grid of sensor tiles
 *
 * Displays all sensors in a grid layout with CO2 values, temperature,
 * humidity, and mini sparkline charts showing recent history.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "sensor_data.h"
#include "inkbird_ble.h"
#include "ui_internal.h"

static const char *TAG = "ui_main";

// ============================================================================
// Private State - Tile UI Elements
// ============================================================================

static lv_obj_t *s_tiles[SENSOR_COUNT];
static lv_obj_t *s_name_labels[SENSOR_COUNT];
static lv_obj_t *s_status_labels[SENSOR_COUNT];
static lv_obj_t *s_co2_labels[SENSOR_COUNT];
static lv_obj_t *s_unit_labels[SENSOR_COUNT];
static lv_obj_t *s_temp_labels[SENSOR_COUNT];
static lv_obj_t *s_hum_labels[SENSOR_COUNT];
static lv_obj_t *s_chart[SENSOR_COUNT];
static lv_obj_t *s_chart_labels[SENSOR_COUNT];
static lv_chart_series_t *s_chart_series[SENSOR_COUNT];
static int32_t s_chart_data[SENSOR_COUNT][SENSOR_HISTORY_SIZE];

// ============================================================================
// Tile Click Handler
// ============================================================================

static void tile_click_cb(lv_event_t *e)
{
    int sensor_idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, ">>> Tile clicked: sensor_idx=%d", sensor_idx);

    if (sensor_idx < 0 || sensor_idx >= SENSOR_COUNT) {
        ESP_LOGW(TAG, "Invalid sensor index, ignoring click");
        return;
    }

    ui_detail_screen_show(sensor_idx);
}

// ============================================================================
// Create Single Tile
// ============================================================================

static void create_tile(uint8_t index, int tile_x, int tile_y, int tile_w, int tile_h)
{
    lv_obj_t *tile = lv_obj_create(g_main_screen);
    lv_obj_set_pos(tile, tile_x, tile_y);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_style_radius(tile, TILE_BORDER_RADIUS, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, TILE_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_TILE_BORDER), 0);
    lv_obj_set_style_pad_all(tile, TILE_PAD, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    s_tiles[index] = tile;

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(name, UI_FONT_LABEL, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, tile_w - (TILE_PAD * 2) - TILE_NAME_WIDTH_MARGIN);
    lv_label_set_text(name, "Sensor");
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    s_name_labels[index] = name;

    lv_obj_t *status = lv_label_create(tile);
    lv_label_set_text(status, "---");
    lv_obj_set_style_text_color(status, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_text_font(status, UI_FONT_SMALL, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(COLOR_OFFLINE), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status, TILE_STATUS_RADIUS, 0);
    lv_obj_set_style_pad_left(status, TILE_STATUS_PAD_H, 0);
    lv_obj_set_style_pad_right(status, TILE_STATUS_PAD_H, 0);
    lv_obj_set_style_pad_top(status, TILE_STATUS_PAD_V, 0);
    lv_obj_set_style_pad_bottom(status, TILE_STATUS_PAD_V, 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, 0, -TILE_STATUS_Y_OFFSET);
    s_status_labels[index] = status;

    lv_obj_t *co2 = lv_label_create(tile);
    lv_obj_set_style_text_color(co2, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(co2, UI_FONT_CO2, 0);
    lv_label_set_text(co2, "---");
    lv_obj_align(co2, LV_ALIGN_CENTER, 0, -TILE_CO2_Y_OFFSET);
    s_co2_labels[index] = co2;

    lv_obj_t *unit = lv_label_create(tile);
    lv_obj_set_style_text_color(unit, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(unit, UI_FONT_SMALL, 0);
    lv_label_set_text(unit, "ppm");
    lv_obj_align_to(unit, co2, LV_ALIGN_OUT_BOTTOM_MID, 0, TILE_UNIT_SPACING);
    s_unit_labels[index] = unit;

    lv_obj_t *temp = lv_label_create(tile);
    lv_obj_set_style_text_color(temp, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(temp, UI_FONT_LABEL, 0);
    lv_label_set_text(temp, "--.-C");
    lv_obj_align(temp, LV_ALIGN_BOTTOM_LEFT, 0, -(CHART_HEIGHT + TILE_UNIT_SPACING));
    s_temp_labels[index] = temp;

    lv_obj_t *hum = lv_label_create(tile);
    lv_obj_set_style_text_color(hum, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(hum, UI_FONT_LABEL, 0);
    lv_label_set_text(hum, "--%");
    lv_obj_align(hum, LV_ALIGN_BOTTOM_RIGHT, 0, -(CHART_HEIGHT + TILE_UNIT_SPACING));
    s_hum_labels[index] = hum;

    lv_obj_t *chart = lv_chart_create(tile);
    lv_obj_set_size(chart, tile_w - (TILE_PAD * 2), CHART_HEIGHT);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(TILE_CHART_BG), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart, TILE_CHART_RADIUS, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, TILE_CHART_PAD, 0);
    lv_obj_set_style_line_width(chart, CHART_LINE_WIDTH, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_line_opa(chart, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_line_color(chart, lv_color_hex(TILE_CHART_GRID_COLOR), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_30, LV_PART_MAIN);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, SENSOR_HISTORY_SIZE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, CHART_MIN_PPM, CHART_MAX_PPM);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    s_chart[index] = chart;

    s_chart_series[index] = lv_chart_add_series(chart, lv_color_hex(COLOR_GOOD), LV_CHART_AXIS_PRIMARY_Y);

    // Initialize chart data to hidden
    for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
        s_chart_data[index][j] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_values(chart, s_chart_series[index], s_chart_data[index], SENSOR_HISTORY_SIZE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *chart_label = lv_label_create(tile);
    lv_obj_set_style_text_color(chart_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(chart_label, UI_FONT_SMALL, 0);
    lv_label_set_text(chart_label, "No history");
    lv_obj_align(chart_label, LV_ALIGN_BOTTOM_MID, 0, -5);
    s_chart_labels[index] = chart_label;
}

// ============================================================================
// Public Functions
// ============================================================================

void ui_main_screen_create(void)
{
    g_main_screen = lv_screen_active();

    lv_obj_clean(g_main_screen);

    lv_obj_set_style_bg_color(g_main_screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_grad_dir(g_main_screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(g_main_screen, LV_OPA_COVER, 0);

    int tile_w = (UI_DISPLAY_WIDTH - GRID_GAP * 3) / GRID_COLS;
    int tile_h = (UI_DISPLAY_HEIGHT - GRID_GAP * 3) / GRID_ROWS;

    for (int i = 0; i < SENSOR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int tile_x = GRID_GAP + col * (tile_w + GRID_GAP);
        int tile_y = GRID_GAP + row * (tile_h + GRID_GAP);
        create_tile(i, tile_x, tile_y, tile_w, tile_h);
    }

    g_ui_created = true;
}

void ui_main_screen_update(void)
{
    static bool s_touch_status_logged = false;
    if (!s_touch_status_logged) {
        ESP_LOGI(TAG, "=== TOUCH STATUS: type=%d (0=none, 1=gt911, 2=xpt2046), indev=%p ===",
                 g_touch_type, (void *)g_ui_indev);
        s_touch_status_logged = true;
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_data_t *sensor = sensor_data_get(i);
        if (sensor == NULL) {
            continue;
        }

        lv_label_set_text(s_name_labels[i], sensor->name);

        co2_status_t status;
        if (!sensor->connected) {
            status = CO2_STATUS_OFFLINE;
        } else {
            inkbird_co2_thresholds_t th = inkbird_ble_get_thresholds(i);
            if (th.thresholds_valid) {
                uint16_t low_ppm, high_ppm;
                if (th.use_custom) {
                    low_ppm = th.plant_low_ppm;
                    high_ppm = th.plant_high_ppm;
                } else {
                    low_ppm = th.normal_low_ppm;
                    high_ppm = th.normal_high_ppm;
                }
                status = sensor_data_get_co2_status_ex(sensor->current.co2_ppm, low_ppm, high_ppm);
            } else {
                status = sensor_data_get_co2_status(sensor->current.co2_ppm);
            }
        }
        const char *status_text = sensor_data_get_status_text(status);
        lv_color_t status_col = ui_status_color(status);

        lv_label_set_text(s_status_labels[i], status_text);
        lv_obj_set_style_text_color(s_status_labels[i], lv_color_hex(COLOR_TEXT_PRIMARY), 0);
        lv_obj_set_style_bg_color(s_status_labels[i], status_col, 0);
        lv_obj_set_style_border_color(s_tiles[i], status_col, 0);
        lv_chart_set_series_color(s_chart[i], s_chart_series[i], status_col);

        // Center area: only show CO2 value or "---" placeholder
        if (sensor->connected && sensor->current.co2_ppm > 0) {
            lv_obj_set_style_text_color(s_co2_labels[i], status_col, 0);
            lv_obj_set_style_text_color(s_unit_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(s_temp_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(s_hum_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_font(s_co2_labels[i], UI_FONT_CO2, 0);

            char co2_str[16];
            snprintf(co2_str, sizeof(co2_str), "%u", sensor->current.co2_ppm);
            lv_label_set_text(s_co2_labels[i], co2_str);

            int temp_whole = sensor->current.temperature / 10;
            int temp_frac = sensor->current.temperature % 10;
            if (temp_frac < 0) {
                temp_frac = -temp_frac;
            }
            char temp_str[24];
            snprintf(temp_str, sizeof(temp_str), "%d.%d\xC2\xB0", temp_whole, temp_frac);
            lv_label_set_text(s_temp_labels[i], temp_str);

            int hum_whole = sensor->current.humidity / 10;
            char hum_str[16];
            snprintf(hum_str, sizeof(hum_str), "%d%%", hum_whole);
            lv_label_set_text(s_hum_labels[i], hum_str);
        } else {
            // No data yet - show placeholder
            lv_obj_set_style_text_color(s_co2_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_unit_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_temp_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_hum_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_font(s_co2_labels[i], UI_FONT_CO2, 0);
            lv_label_set_text(s_co2_labels[i], "---");
            lv_label_set_text(s_temp_labels[i], "");
            lv_label_set_text(s_hum_labels[i], "");
        }

        // Bottom area: chart or status messages
        uint8_t history_count = 0;
        const int16_t *history = sensor_data_get_co2_history_filtered(i, 60, &history_count);
        bool has_chart_data = (history_count > 1 && history != NULL);

        // Priority 1: Syncing progress
        if (sensor->downloading) {
            char sync_str[24];
            uint16_t expected = 0, received = 0;
            inkbird_ble_get_history_progress(&expected, &received);
            if (expected > 0) {
                int percent = (received * 100) / expected;
                if (percent > 100) {
                    percent = 100;
                }
                snprintf(sync_str, sizeof(sync_str), "Syncing %d%%", percent);
            } else {
                snprintf(sync_str, sizeof(sync_str), "Preparing...");
            }
            lv_label_set_text(s_chart_labels[i], sync_str);
            lv_obj_clear_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        // Priority 2: Show chart if data available
        if (has_chart_data) {
            for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
                s_chart_data[i][j] = history[j];
            }
            lv_chart_set_series_values(s_chart[i], s_chart_series[i], s_chart_data[i], SENSOR_HISTORY_SIZE);
            lv_obj_add_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        // Priority 3: Activity status
        if (sensor->activity_status[0] != '\0') {
            lv_label_set_text(s_chart_labels[i], sensor->activity_status);
            lv_obj_clear_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        // Priority 4: "No data" fallback
        lv_label_set_text(s_chart_labels[i], "No data");
        lv_obj_clear_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
    }
}

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "epd_driver.h"
#include "sensor_data.h"
#include "ui_co2_display.h"
#include "inkbird_ble.h"

#define GRID_COLS 2
#define GRID_ROWS 2
#define GRID_GAP 6
#define TILE_PAD 12
#define CHART_HEIGHT 42
#define HEADER_HEIGHT 20
#define CHART_MIN_PPM 400
#define CHART_MAX_PPM 2000
#define LVGL_TICK_PERIOD_MS 5
#define LVGL_BUFFER_LINES 20

#define COLOR_BG_DARK       0x1A1A2E
#define COLOR_TILE_BG       0x16213E
#define COLOR_TILE_BORDER   0x0F3460
#define COLOR_TEXT_PRIMARY  0xFFFFFF
#define COLOR_TEXT_MUTED    0x8892A0
#define COLOR_GOOD          0x00D26A
#define COLOR_MODERATE      0xFFD93D
#define COLOR_WARNING       0xFF8C32
#define COLOR_ALERT         0xFF4757
#define COLOR_OFFLINE       0x4A5568


static const char *TAG = "ui";

static lv_display_t *s_display = NULL;
static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;
static uint8_t *s_rotate_buf = NULL;
static esp_timer_handle_t s_tick_timer = NULL;

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
static bool s_ui_created = false;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void rgb565_to_bgr565_swap(uint16_t *buf, uint32_t px_count)
{
    for (uint32_t i = 0; i < px_count; i++) {
        uint16_t px = buf[i];
        uint16_t r = (px >> 11) & 0x1F;
        uint16_t g = (px >> 5) & 0x3F;
        uint16_t b = px & 0x1F;
        uint16_t bgr = (b << 11) | (g << 5) | r;
        buf[i] = (bgr >> 8) | (bgr << 8);
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t px_count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    rgb565_to_bgr565_swap((uint16_t *)px_map, px_count);
    epd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static lv_color_t status_color(co2_status_t status)
{
    switch (status) {
    case CO2_STATUS_GOOD:
        return lv_color_hex(COLOR_GOOD);
    case CO2_STATUS_MODERATE:
        return lv_color_hex(COLOR_MODERATE);
    case CO2_STATUS_WARNING:
        return lv_color_hex(COLOR_WARNING);
    case CO2_STATUS_ALERT:
        return lv_color_hex(COLOR_ALERT);
    default:
        return lv_color_hex(COLOR_OFFLINE);
    }
}

static void create_tile(uint8_t index, int tile_x, int tile_y, int tile_w, int tile_h)
{
    lv_obj_t *tile = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(tile, tile_x, tile_y);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_TILE_BORDER), 0);
    lv_obj_set_style_pad_all(tile, TILE_PAD, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    s_tiles[index] = tile;

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, tile_w - (TILE_PAD * 2) - 50);
    lv_label_set_text(name, "Sensor");
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    s_name_labels[index] = name;

    lv_obj_t *status = lv_label_create(tile);
    lv_label_set_text(status, "---");
    lv_obj_set_style_text_color(status, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(COLOR_OFFLINE), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status, 4, 0);
    lv_obj_set_style_pad_left(status, 6, 0);
    lv_obj_set_style_pad_right(status, 6, 0);
    lv_obj_set_style_pad_top(status, 2, 0);
    lv_obj_set_style_pad_bottom(status, 2, 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, 0, -2);
    s_status_labels[index] = status;

    lv_obj_t *co2 = lv_label_create(tile);
    lv_obj_set_style_text_color(co2, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(co2, &lv_font_montserrat_48, 0);
    lv_label_set_text(co2, "---");
    lv_obj_align(co2, LV_ALIGN_CENTER, 0, -8);
    s_co2_labels[index] = co2;

    lv_obj_t *unit = lv_label_create(tile);
    lv_obj_set_style_text_color(unit, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(unit, "ppm");
    lv_obj_align_to(unit, co2, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    s_unit_labels[index] = unit;

    lv_obj_t *temp = lv_label_create(tile);
    lv_obj_set_style_text_color(temp, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(temp, &lv_font_montserrat_14, 0);
    lv_label_set_text(temp, "--.-C");
    lv_obj_align(temp, LV_ALIGN_BOTTOM_LEFT, 0, -(CHART_HEIGHT + 6));
    s_temp_labels[index] = temp;

    lv_obj_t *hum = lv_label_create(tile);
    lv_obj_set_style_text_color(hum, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(hum, &lv_font_montserrat_14, 0);
    lv_label_set_text(hum, "--%");
    lv_obj_align(hum, LV_ALIGN_BOTTOM_RIGHT, 0, -(CHART_HEIGHT + 6));
    s_hum_labels[index] = hum;

    lv_obj_t *chart = lv_chart_create(tile);
    lv_obj_set_size(chart, tile_w - (TILE_PAD * 2), CHART_HEIGHT);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart, 6, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 4, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A3F5F), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_30, LV_PART_MAIN);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, SENSOR_HISTORY_SIZE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, CHART_MIN_PPM, CHART_MAX_PPM);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    s_chart[index] = chart;

    s_chart_series[index] = lv_chart_add_series(chart, lv_color_hex(COLOR_GOOD), LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *chart_label = lv_label_create(tile);
    lv_obj_set_style_text_color(chart_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(chart_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(chart_label, "No history");
    lv_obj_align(chart_label, LV_ALIGN_BOTTOM_MID, 0, -5);
    s_chart_labels[index] = chart_label;
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    int tile_w = (480 - GRID_GAP * 3) / GRID_COLS;
    int tile_h = (320 - GRID_GAP * 3) / GRID_ROWS;

    for (int i = 0; i < SENSOR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int tile_x = GRID_GAP + col * (tile_w + GRID_GAP);
        int tile_y = GRID_GAP + row * (tile_h + GRID_GAP);
        create_tile(i, tile_x, tile_y, tile_w, tile_h);
    }

    s_ui_created = true;
}

void ui_co2_display_init(void)
{
    lv_init();

    s_display = lv_display_create(480, 320);
    lv_display_set_default(s_display);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
    lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_0);

    lv_theme_t *theme = lv_theme_default_init(
        s_display,
        lv_color_hex(0x1E1E1E),
        lv_color_hex(0x2A2A2A),
        true,
        &lv_font_montserrat_14
    );
    lv_display_set_theme(s_display, theme);

    size_t buf_pixels = 480 * LVGL_BUFFER_LINES;
    size_t buf_size = buf_pixels * sizeof(lv_color_t);

    s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf1 == NULL) {
        s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    if (s_buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer");
        return;
    }

    s_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf2 == NULL) {
        s_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    if (s_buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL second buffer");
        return;
    }

    lv_display_set_buffers(s_display, s_buf1, s_buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_rotate_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_rotate_buf == NULL) {
        s_rotate_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    if (s_rotate_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate rotation buffer");
        return;
    }

    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick"
    };
    if (esp_timer_create(&tick_args, &s_tick_timer) == ESP_OK) {
        esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_MS * 1000);
    } else {
        ESP_LOGE(TAG, "Failed to start LVGL tick timer");
    }
}

void ui_co2_display_loading(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    s_ui_created = false;

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "CO2 Display");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_label_set_text(subtitle, "Connecting to sensors...");
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 14);
}

void ui_co2_display_update(void)
{
    if (!s_ui_created) {
        create_ui();
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_data_t *sensor = sensor_data_get(i);
        if (sensor == NULL) {
            continue;
        }

        lv_label_set_text(s_name_labels[i], sensor->name);

        co2_status_t status = sensor_data_get_co2_status(sensor->current.co2_ppm);
        const char *status_text = sensor_data_get_status_text(status);
        lv_color_t status_col = status_color(status);

        lv_label_set_text(s_status_labels[i], status_text);
        lv_obj_set_style_text_color(s_status_labels[i], lv_color_hex(COLOR_TEXT_PRIMARY), 0);
        lv_obj_set_style_bg_color(s_status_labels[i], status_col, 0);
        lv_obj_set_style_border_color(s_tiles[i], status_col, 0);
        lv_chart_set_series_color(s_chart[i], s_chart_series[i], status_col);

        if (sensor->connected) {
            lv_obj_set_style_text_color(s_co2_labels[i], status_col, 0);
            lv_obj_set_style_text_color(s_unit_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(s_temp_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(s_hum_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);

            char co2_str[16];
            snprintf(co2_str, sizeof(co2_str), "%u", sensor->current.co2_ppm);
            lv_label_set_text(s_co2_labels[i], co2_str);

            int temp_whole = sensor->current.temperature / 10;
            int temp_frac = sensor->current.temperature % 10;
            if (temp_frac < 0) {
                temp_frac = -temp_frac;
            }
            char temp_str[24];
            snprintf(temp_str, sizeof(temp_str), "%d.%dC", temp_whole, temp_frac);
            lv_label_set_text(s_temp_labels[i], temp_str);

            int hum_whole = sensor->current.humidity / 10;
            char hum_str[16];
            snprintf(hum_str, sizeof(hum_str), "%d%%", hum_whole);
            lv_label_set_text(s_hum_labels[i], hum_str);
        } else {
            lv_obj_set_style_text_color(s_co2_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_unit_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_temp_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_hum_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_label_set_text(s_co2_labels[i], "---");
            lv_label_set_text(s_temp_labels[i], "--.-C");
            lv_label_set_text(s_hum_labels[i], "--%");
        }

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
                snprintf(sync_str, sizeof(sync_str), "Syncing...");
            }
            lv_label_set_text(s_chart_labels[i], sync_str);
            lv_obj_clear_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        uint8_t history_count = 0;
        const int16_t *history = sensor_data_get_co2_history(i, &history_count);
        if (history_count > 1 && history != NULL) {
            for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
                s_chart_data[i][j] = history[j];
            }
            lv_chart_set_series_values(s_chart[i], s_chart_series[i], s_chart_data[i], SENSOR_HISTORY_SIZE);
            lv_obj_add_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_chart_labels[i], "No data");
            lv_obj_clear_flag(s_chart_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chart[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_co2_display_force_refresh(void)
{
    if (s_display == NULL) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        lv_obj_invalidate(screen);
    }

    lv_refr_now(s_display);
}

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
#include "gt911_touch.h"
#include "xpt2046_touch.h"

typedef enum {
    TOUCH_TYPE_NONE,
    TOUCH_TYPE_GT911,
    TOUCH_TYPE_XPT2046
} touch_type_t;

static touch_type_t s_touch_type = TOUCH_TYPE_NONE;

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

#define DETAIL_CHART_HEIGHT 80
#define DETAIL_PAD 10

#define DETAIL_CHART_Y_MIN 400
#define DETAIL_CHART_Y_MAX 1600
#define DETAIL_CHART_Y_RANGE (DETAIL_CHART_Y_MAX - DETAIL_CHART_Y_MIN)

#define TEMP_MIN_TENTHS 150
#define TEMP_MAX_TENTHS 300
#define TEMP_RANGE_TENTHS (TEMP_MAX_TENTHS - TEMP_MIN_TENTHS)

#define HUM_MIN_PERCENT 20
#define HUM_MAX_PERCENT 80
#define HUM_RANGE_PERCENT (HUM_MAX_PERCENT - HUM_MIN_PERCENT)

#define PRES_MIN_HPA 950
#define PRES_MAX_HPA 1050
#define PRES_RANGE_HPA (PRES_MAX_HPA - PRES_MIN_HPA)

#define CO2_MIN_PPM 400
#define CO2_MAX_PPM 1600

#define COLOR_BG_DARK       0x0D1117
#define COLOR_TILE_BG       0x161B22
#define COLOR_TILE_BORDER   0x0F3460
#define COLOR_TEXT_PRIMARY  0xE6EDF3
#define COLOR_TEXT_MUTED    0x8B949E
#define COLOR_TEXT_DIM      0x586069
#define COLOR_GOOD          0x3FB950
#define COLOR_MODERATE      0xFFD93D
#define COLOR_WARNING       0xFF8C32
#define COLOR_ALERT         0xFF4757
#define COLOR_OFFLINE       0x4A5568
#define COLOR_TEMP          0xFFA657
#define COLOR_HUMIDITY      0x60CDE4
#define COLOR_PRESSURE      0xB48CFF
#define COLOR_ACCENT_BLUE   0x58A6FF
#define COLOR_BG_CHART      0x11151C
#define COLOR_GRID_LINE     0x232830

static const char *TAG = "ui";

static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev = NULL;
static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;
static uint8_t *s_rotate_buf = NULL;
static esp_timer_handle_t s_tick_timer = NULL;

static lv_obj_t *s_main_screen = NULL;
static lv_obj_t *s_detail_screen = NULL;
static int s_selected_sensor = -1;

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
static lv_obj_t *s_loading_status_label = NULL;

static lv_obj_t *s_detail_name_label = NULL;
static lv_obj_t *s_detail_co2_card = NULL;
static lv_obj_t *s_detail_co2_value = NULL;
static lv_obj_t *s_detail_temp_card = NULL;
static lv_obj_t *s_detail_temp_value = NULL;
static lv_obj_t *s_detail_hum_card = NULL;
static lv_obj_t *s_detail_hum_value = NULL;
static lv_obj_t *s_detail_pres_card = NULL;
static lv_obj_t *s_detail_pres_value = NULL;
static lv_obj_t *s_detail_chart = NULL;
static lv_chart_series_t *s_detail_co2_series = NULL;
static lv_chart_series_t *s_detail_temp_series = NULL;
static lv_chart_series_t *s_detail_hum_series = NULL;
static lv_chart_series_t *s_detail_pres_series = NULL;
static int32_t s_detail_co2_data[SENSOR_HISTORY_SIZE];
static int32_t s_detail_temp_data[SENSOR_HISTORY_SIZE];
static int32_t s_detail_hum_data[SENSOR_HISTORY_SIZE];
static int32_t s_detail_pres_data[SENSOR_HISTORY_SIZE];

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

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;

    if (s_touch_type == TOUCH_TYPE_GT911) {
        gt911_touch_data_t touch;
        if (gt911_read(&touch) && touch.pressed) {
            data->point.x = touch.x;
            data->point.y = touch.y;
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGI(TAG, "GT911 touch at (%d, %d)", touch.x, touch.y);
        }
    } else if (s_touch_type == TOUCH_TYPE_XPT2046) {
        xpt2046_touch_data_t touch;
        if (xpt2046_read(&touch) && touch.pressed) {
            data->point.x = touch.x;
            data->point.y = touch.y;
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGI(TAG, "XPT2046 touch at (%d, %d)", touch.x, touch.y);
        }
    }

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
    case CO2_STATUS_OFFLINE:
    default:
        return lv_color_hex(COLOR_OFFLINE);
    }
}

static void back_btn_event_cb(lv_event_t *e)
{
    (void)e;
    s_selected_sensor = -1;
    lv_screen_load(s_main_screen);
}

static void update_detail_screen(int sensor_idx)
{
    if (s_detail_screen == NULL || sensor_idx < 0 || sensor_idx >= SENSOR_COUNT) {
        return;
    }

    sensor_data_t *sensor = sensor_data_get(sensor_idx);
    if (sensor == NULL) {
        return;
    }

    lv_label_set_text(s_detail_name_label, sensor->name);

    co2_status_t status;
    if (!sensor->connected) {
        status = CO2_STATUS_OFFLINE;
    } else {
        inkbird_co2_thresholds_t th = inkbird_ble_get_thresholds(sensor_idx);
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

    lv_color_t col = status_color(status);

    if (sensor->connected) {
        char co2_str[24];
        snprintf(co2_str, sizeof(co2_str), "%u ppm", sensor->current.co2_ppm);
        lv_label_set_text(s_detail_co2_value, co2_str);

        int temp_whole = sensor->current.temperature / 10;
        int temp_frac = sensor->current.temperature % 10;
        if (temp_frac < 0) temp_frac = -temp_frac;
        char temp_str[24];
        snprintf(temp_str, sizeof(temp_str), "%d.%d \xC2\xB0" "C", temp_whole, temp_frac);
        lv_label_set_text(s_detail_temp_value, temp_str);

        int hum_whole = sensor->current.humidity / 10;
        int hum_frac = sensor->current.humidity % 10;
        char hum_str[24];
        snprintf(hum_str, sizeof(hum_str), "%d.%d %%", hum_whole, hum_frac);
        lv_label_set_text(s_detail_hum_value, hum_str);

        char pres_str[24];
        snprintf(pres_str, sizeof(pres_str), "%u hPa", sensor->current.pressure);
        lv_label_set_text(s_detail_pres_value, pres_str);
    } else {
        lv_label_set_text(s_detail_co2_value, "--- ppm");
        lv_label_set_text(s_detail_temp_value, "--.- \xC2\xB0" "C");
        lv_label_set_text(s_detail_hum_value, "--.- %");
        lv_label_set_text(s_detail_pres_value, "---- hPa");
    }

    uint8_t history_count = 0;
    const int16_t *co2_hist = sensor_data_get_co2_history(sensor_idx, &history_count);
    if (history_count > 1 && co2_hist != NULL) {
        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            s_detail_co2_data[j] = co2_hist[j];
        }
        lv_chart_set_series_values(s_detail_chart, s_detail_co2_series, s_detail_co2_data, SENSOR_HISTORY_SIZE);
        lv_chart_set_series_color(s_detail_chart, s_detail_co2_series, col);
    }

    const int16_t *temp_hist = sensor_data_get_temp_history(sensor_idx, &history_count);
    if (history_count > 1 && temp_hist != NULL) {
        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            int16_t t = temp_hist[j];
            int32_t val;
            if (t <= 0 || t < TEMP_MIN_TENTHS - 50 || t > TEMP_MAX_TENTHS + 100) {
                val = (DETAIL_CHART_Y_MIN + DETAIL_CHART_Y_MAX) / 2;
            } else {
                val = DETAIL_CHART_Y_MIN + ((t - TEMP_MIN_TENTHS) * DETAIL_CHART_Y_RANGE / TEMP_RANGE_TENTHS);
            }
            if (val < DETAIL_CHART_Y_MIN) val = DETAIL_CHART_Y_MIN;
            if (val > DETAIL_CHART_Y_MAX) val = DETAIL_CHART_Y_MAX;
            s_detail_temp_data[j] = val;
        }
        lv_chart_set_series_values(s_detail_chart, s_detail_temp_series, s_detail_temp_data, SENSOR_HISTORY_SIZE);
    }

    const int16_t *hum_hist = sensor_data_get_hum_history(sensor_idx, &history_count);
    if (history_count > 1 && hum_hist != NULL) {
        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            int16_t h = hum_hist[j];
            int32_t val;
            if (h <= 0 || h > 100) {
                val = (DETAIL_CHART_Y_MIN + DETAIL_CHART_Y_MAX) / 2;
            } else {
                val = DETAIL_CHART_Y_MIN + ((h - HUM_MIN_PERCENT) * DETAIL_CHART_Y_RANGE / HUM_RANGE_PERCENT);
            }
            if (val < DETAIL_CHART_Y_MIN) val = DETAIL_CHART_Y_MIN;
            if (val > DETAIL_CHART_Y_MAX) val = DETAIL_CHART_Y_MAX;
            s_detail_hum_data[j] = val;
        }
        lv_chart_set_series_values(s_detail_chart, s_detail_hum_series, s_detail_hum_data, SENSOR_HISTORY_SIZE);
    }

    const int16_t *pres_hist = sensor_data_get_pres_history(sensor_idx, &history_count);
    if (history_count > 1 && pres_hist != NULL) {
        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            int16_t p = pres_hist[j];
            int32_t val;
            if (p <= 0 || p < PRES_MIN_HPA || p > PRES_MAX_HPA) {
                val = (DETAIL_CHART_Y_MIN + DETAIL_CHART_Y_MAX) / 2;
            } else {
                val = DETAIL_CHART_Y_MIN + ((p - PRES_MIN_HPA) * DETAIL_CHART_Y_RANGE / PRES_RANGE_HPA);
            }
            if (val < DETAIL_CHART_Y_MIN) val = DETAIL_CHART_Y_MIN;
            if (val > DETAIL_CHART_Y_MAX) val = DETAIL_CHART_Y_MAX;
            s_detail_pres_data[j] = val;
        }
        lv_chart_set_series_values(s_detail_chart, s_detail_pres_series, s_detail_pres_data, SENSOR_HISTORY_SIZE);
    }
}

static lv_obj_t *create_metric_card(lv_obj_t *parent, int x, int y, int w, int h,
                                     uint32_t color, const char *label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_set_pos(bar, 4, 3);
    lv_obj_set_size(bar, w - 8, 3);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl, label);
    lv_obj_set_pos(lbl, 6, h - 18);

    return card;
}

static void create_detail_screen(void)
{
    s_detail_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_detail_screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(s_detail_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_detail_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_label_create(s_detail_screen);
    lv_label_set_text(back_btn, "< Back");
    lv_obj_set_style_text_color(back_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(back_btn, 16, 10);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    s_detail_name_label = lv_label_create(s_detail_screen);
    lv_obj_set_style_text_color(s_detail_name_label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_name_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_name_label, "Sensor");
    lv_obj_align(s_detail_name_label, LV_ALIGN_TOP_MID, 0, 8);

    int cards_y = 36;
    int card_h = 52;
    int card_w = 108;
    int gap = 8;
    int total_w = (card_w * 4) + (gap * 3);
    int start_x = (480 - total_w) / 2;

    s_detail_co2_card = create_metric_card(s_detail_screen, start_x, cards_y, card_w, card_h, COLOR_GOOD, "CO2");
    s_detail_co2_value = lv_label_create(s_detail_co2_card);
    lv_obj_set_style_text_color(s_detail_co2_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_co2_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_co2_value, "--- ppm");
    lv_obj_set_pos(s_detail_co2_value, 6, 12);

    s_detail_temp_card = create_metric_card(s_detail_screen, start_x + card_w + gap, cards_y, card_w, card_h, COLOR_TEMP, "TEMP");
    s_detail_temp_value = lv_label_create(s_detail_temp_card);
    lv_obj_set_style_text_color(s_detail_temp_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_temp_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_temp_value, "--.- \xC2\xB0" "C");
    lv_obj_set_pos(s_detail_temp_value, 6, 12);

    s_detail_hum_card = create_metric_card(s_detail_screen, start_x + 2 * (card_w + gap), cards_y, card_w, card_h, COLOR_HUMIDITY, "HUMIDITY");
    s_detail_hum_value = lv_label_create(s_detail_hum_card);
    lv_obj_set_style_text_color(s_detail_hum_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_hum_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_hum_value, "--.- %");
    lv_obj_set_pos(s_detail_hum_value, 6, 12);

    s_detail_pres_card = create_metric_card(s_detail_screen, start_x + 3 * (card_w + gap), cards_y, card_w, card_h, COLOR_PRESSURE, "PRESSURE");
    s_detail_pres_value = lv_label_create(s_detail_pres_card);
    lv_obj_set_style_text_color(s_detail_pres_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_pres_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_pres_value, "---- hPa");
    lv_obj_set_pos(s_detail_pres_value, 6, 12);

    int chart_container_top = 92;
    int chart_container_h = 320 - chart_container_top - 8;

    lv_obj_t *chart_container = lv_obj_create(s_detail_screen);
    lv_obj_set_pos(chart_container, 12, chart_container_top);
    lv_obj_set_size(chart_container, 480 - 24, chart_container_h);
    lv_obj_set_style_bg_color(chart_container, lv_color_hex(COLOR_BG_CHART), 0);
    lv_obj_set_style_bg_opa(chart_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart_container, 8, 0);
    lv_obj_set_style_border_width(chart_container, 0, 0);
    lv_obj_set_style_pad_all(chart_container, 0, 0);
    lv_obj_clear_flag(chart_container, LV_OBJ_FLAG_SCROLLABLE);

    int plot_top = 8;
    int plot_bottom = chart_container_h - 20;
    int plot_left = 32;
    int plot_right = 480 - 24 - 28;

    s_detail_chart = lv_chart_create(chart_container);
    lv_obj_set_pos(s_detail_chart, plot_left, plot_top);
    lv_obj_set_size(s_detail_chart, plot_right - plot_left, plot_bottom - plot_top);
    lv_obj_set_style_bg_color(s_detail_chart, lv_color_hex(COLOR_BG_CHART), 0);
    lv_obj_set_style_bg_opa(s_detail_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_detail_chart, 0, 0);
    lv_obj_set_style_border_width(s_detail_chart, 0, 0);
    lv_obj_set_style_pad_all(s_detail_chart, 0, 0);
    lv_obj_set_style_line_width(s_detail_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_detail_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_detail_chart, lv_color_hex(COLOR_GRID_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_detail_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_chart_set_type(s_detail_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_detail_chart, SENSOR_HISTORY_SIZE);
    lv_chart_set_div_line_count(s_detail_chart, 4, 4);
    lv_chart_set_axis_range(s_detail_chart, LV_CHART_AXIS_PRIMARY_Y, DETAIL_CHART_Y_MIN, DETAIL_CHART_Y_MAX);
    lv_obj_clear_flag(s_detail_chart, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_pres_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_PRESSURE), LV_CHART_AXIS_PRIMARY_Y);
    s_detail_hum_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_HUMIDITY), LV_CHART_AXIS_PRIMARY_Y);
    s_detail_temp_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_TEMP), LV_CHART_AXIS_PRIMARY_Y);
    s_detail_co2_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_GOOD), LV_CHART_AXIS_PRIMARY_Y);

    int chart_h = plot_bottom - plot_top;
    int y_div = chart_h / 4;
    const char *y_labels[] = {"1.6k", "1.3k", "1k", "700", "400"};
    uint32_t co2_colors[] = {0xFF4757, 0xFF8C32, 0xFFD93D, 0x7ED321, 0x3FB950};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *y_lbl = lv_label_create(chart_container);
        lv_obj_set_style_text_color(y_lbl, lv_color_hex(co2_colors[i]), 0);
        lv_obj_set_style_text_font(y_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(y_lbl, y_labels[i]);
        lv_obj_set_pos(y_lbl, 2, plot_top + i * y_div - 6);
    }

    const char *y_temp_labels[] = {"30", "26", "22", "18", "15"};
    uint32_t temp_colors[] = {0xFF8C32, 0xFFD93D, 0x7ED321, 0x60CDE4, 0x29B6F6};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *yt_lbl = lv_label_create(chart_container);
        lv_obj_set_style_text_color(yt_lbl, lv_color_hex(temp_colors[i]), 0);
        lv_obj_set_style_text_font(yt_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(yt_lbl, y_temp_labels[i]);
        lv_obj_set_pos(yt_lbl, plot_right + 4, plot_top + i * y_div - 6);
    }

    int chart_w = plot_right - plot_left;
    lv_obj_t *x_lbl1 = lv_label_create(chart_container);
    lv_obj_set_style_text_color(x_lbl1, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(x_lbl1, &lv_font_montserrat_12, 0);
    lv_label_set_text(x_lbl1, "-60m");
    lv_obj_set_pos(x_lbl1, plot_left, plot_bottom + 4);

    lv_obj_t *x_lbl2 = lv_label_create(chart_container);
    lv_obj_set_style_text_color(x_lbl2, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(x_lbl2, &lv_font_montserrat_12, 0);
    lv_label_set_text(x_lbl2, "-30m");
    lv_obj_set_pos(x_lbl2, plot_left + chart_w / 2 - 15, plot_bottom + 4);

    lv_obj_t *x_lbl3 = lv_label_create(chart_container);
    lv_obj_set_style_text_color(x_lbl3, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(x_lbl3, &lv_font_montserrat_12, 0);
    lv_label_set_text(x_lbl3, "now");
    lv_obj_set_pos(x_lbl3, plot_right - 24, plot_bottom + 4);

}

static void tile_click_cb(lv_event_t *e)
{
    int sensor_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (sensor_idx < 0 || sensor_idx >= SENSOR_COUNT) {
        return;
    }

    s_selected_sensor = sensor_idx;

    if (s_detail_screen == NULL) {
        create_detail_screen();
    }

    update_detail_screen(sensor_idx);
    lv_screen_load(s_detail_screen);
}

static void create_tile(uint8_t index, int tile_x, int tile_y, int tile_w, int tile_h)
{
    lv_obj_t *tile = lv_obj_create(s_main_screen);
    lv_obj_set_pos(tile, tile_x, tile_y);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_TILE_BORDER), 0);
    lv_obj_set_style_pad_all(tile, TILE_PAD, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
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
    lv_obj_align(co2, LV_ALIGN_CENTER, 0, -15);
    s_co2_labels[index] = co2;

    lv_obj_t *unit = lv_label_create(tile);
    lv_obj_set_style_text_color(unit, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_12, 0);
    lv_label_set_text(unit, "ppm");
    lv_obj_align_to(unit, co2, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
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
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
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
    s_loading_status_label = NULL;

    s_main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_main_screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_grad_dir(s_main_screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(s_main_screen, LV_OPA_COVER, 0);

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

    esp_err_t ret = gt911_init();
    if (ret == ESP_OK) {
        s_touch_type = TOUCH_TYPE_GT911;
        ESP_LOGI(TAG, "GT911 capacitive touch initialized");
    } else {
        ESP_LOGW(TAG, "GT911 init failed, trying XPT2046...");
        ret = xpt2046_init();
        if (ret == ESP_OK) {
            s_touch_type = TOUCH_TYPE_XPT2046;
            ESP_LOGI(TAG, "XPT2046 resistive touch initialized");
        } else {
            ESP_LOGW(TAG, "XPT2046 init failed, touch disabled");
        }
    }

    ESP_LOGI(TAG, ">>> Touch type after init: %d (0=none, 1=gt911, 2=xpt2046)", s_touch_type);

    if (s_touch_type != TOUCH_TYPE_NONE) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        ESP_LOGI(TAG, "Touch input device registered");
    } else {
        ESP_LOGW(TAG, ">>> No touch controller available - touch disabled!");
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

    s_loading_status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_loading_status_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(s_loading_status_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_loading_status_label, "Initializing...");
    lv_obj_align(s_loading_status_label, LV_ALIGN_CENTER, 0, 14);
}

void ui_co2_display_set_status(const char *status)
{
    if (s_loading_status_label != NULL) {
        lv_label_set_text(s_loading_status_label, status);
        lv_obj_align(s_loading_status_label, LV_ALIGN_CENTER, 0, 14);
    }
}

void ui_co2_display_update(void)
{
    static bool s_touch_status_logged = false;
    if (!s_touch_status_logged) {
        ESP_LOGI(TAG, "=== TOUCH STATUS: type=%d (0=none, 1=gt911, 2=xpt2046), indev=%p ===",
                 s_touch_type, (void*)s_indev);
        s_touch_status_logged = true;
    }

    if (!s_ui_created) {
        create_ui();
    }

    lv_obj_t *active_screen = lv_screen_active();

    if (active_screen == s_detail_screen && s_selected_sensor >= 0) {
        update_detail_screen(s_selected_sensor);
        return;
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
            lv_obj_set_style_text_font(s_co2_labels[i], &lv_font_montserrat_48, 0);

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
        } else if (sensor->status_text[0] != '\0') {
            lv_obj_set_style_text_color(s_co2_labels[i], lv_color_hex(COLOR_TEXT_MUTED), 0);
            lv_obj_set_style_text_color(s_unit_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_temp_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_hum_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_font(s_co2_labels[i], &lv_font_montserrat_14, 0);
            lv_label_set_text(s_co2_labels[i], sensor->status_text);
            lv_label_set_text(s_temp_labels[i], "");
            lv_label_set_text(s_hum_labels[i], "");
        } else {
            lv_obj_set_style_text_color(s_co2_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_unit_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_temp_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_color(s_hum_labels[i], lv_color_hex(COLOR_OFFLINE), 0);
            lv_obj_set_style_text_font(s_co2_labels[i], &lv_font_montserrat_48, 0);
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

int ui_co2_display_get_touch_type(void)
{
    return (int)s_touch_type;
}

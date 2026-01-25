/**
 * @file ui_detail_screen.c
 * @brief Detail screen with full sensor information and multi-metric chart
 *
 * Shows detailed view of a single sensor including CO2, temperature,
 * humidity, pressure readings and a multi-line chart with axis labels.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

#include "sensor_data.h"
#include "inkbird_ble.h"
#include "detail_history.h"
#include "ui_internal.h"

static const char *TAG = "ui_detail";

// Debug macro for heap monitoring
#define LOG_HEAP(label) do { \
    ESP_LOGI(TAG, "HEAP[%s]: free=%u largest=%u min=%u", label, \
        (unsigned)esp_get_free_heap_size(), \
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), \
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)); \
} while(0)

// ============================================================================
// Private State - Detail Screen Elements
// ============================================================================

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
static lv_obj_t *s_detail_chart_container = NULL;

static lv_chart_series_t *s_detail_co2_series = NULL;
static lv_chart_series_t *s_detail_temp_series = NULL;
static lv_chart_series_t *s_detail_hum_series = NULL;
static lv_chart_series_t *s_detail_pres_series = NULL;

static int32_t s_detail_co2_data[SENSOR_HISTORY_SIZE];
static int32_t s_detail_temp_data[SENSOR_HISTORY_SIZE];
static int32_t s_detail_hum_data[SENSOR_HISTORY_SIZE];
static int32_t s_detail_pres_data[SENSOR_HISTORY_SIZE];

static lv_obj_t *s_detail_y_labels_co2[5];
static lv_obj_t *s_detail_y_labels_temp[5];
static lv_obj_t *s_detail_y_labels_hum[5];
static lv_obj_t *s_detail_y_labels_pres[5];
static lv_obj_t *s_detail_x_labels[3] = {NULL, NULL, NULL};
static int s_y_label_positions[5];
static int s_plot_left_x = 2;
static int s_plot_right_x = 0;

// Loading overlay elements
static lv_obj_t *s_loading_overlay = NULL;
static lv_obj_t *s_loading_spinner = NULL;
static lv_obj_t *s_loading_progress_label = NULL;
static lv_obj_t *s_loading_cancel_btn = NULL;
static detail_history_state_t s_last_download_state = DETAIL_HISTORY_IDLE;

// ============================================================================
// Metric Card Creation Helper
// ============================================================================

static lv_obj_t *create_metric_card(lv_obj_t *parent, int x, int y, int w, int h,
                                     uint32_t color, const char *label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, DETAIL_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_set_pos(bar, DETAIL_CARD_BAR_X, DETAIL_CARD_BAR_Y);
    lv_obj_set_size(bar, w - DETAIL_CARD_BAR_MARGIN, DETAIL_CARD_BAR_HEIGHT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, DETAIL_CARD_BAR_RADIUS, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl, label);
    lv_obj_set_pos(lbl, DETAIL_CARD_LABEL_X, h - DETAIL_CARD_LABEL_Y_OFFSET);

    return card;
}

// ============================================================================
// Metric Selection Logic
// ============================================================================

static void update_metric_selection(void)
{
    lv_obj_t *cards[] = {s_detail_co2_card, s_detail_temp_card, s_detail_hum_card, s_detail_pres_card};
    uint32_t colors[] = {COLOR_GOOD, COLOR_TEMP, COLOR_HUMIDITY, COLOR_PRESSURE};
    lv_chart_series_t *series[] = {s_detail_co2_series, s_detail_temp_series, s_detail_hum_series, s_detail_pres_series};

    for (int i = 0; i < 4; i++) {
        if (cards[i] == NULL) continue;

        if (g_selected_metric == i) {
            lv_obj_set_style_border_width(cards[i], TILE_BORDER_WIDTH, 0);
            lv_obj_set_style_border_color(cards[i], lv_color_hex(colors[i]), 0);
        } else {
            lv_obj_set_style_border_width(cards[i], 0, 0);
        }
    }

    static int32_t hidden_data[SENSOR_HISTORY_SIZE];
    static bool hidden_data_init = false;
    if (!hidden_data_init) {
        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            hidden_data[j] = LV_CHART_POINT_NONE;
        }
        hidden_data_init = true;
    }

    int32_t *data_arrays[] = {s_detail_co2_data, s_detail_temp_data, s_detail_hum_data, s_detail_pres_data};

    for (int i = 0; i < 4; i++) {
        if (series[i] == NULL) continue;

        bool should_hide = (g_selected_metric != -1 && g_selected_metric != i);
        if (should_hide) {
            lv_chart_set_series_values(s_detail_chart, series[i], hidden_data, SENSOR_HISTORY_SIZE);
        } else {
            lv_chart_set_series_values(s_detail_chart, series[i], data_arrays[i], SENSOR_HISTORY_SIZE);
        }
    }

    lv_obj_t **all_labels[] = {s_detail_y_labels_co2, s_detail_y_labels_temp, s_detail_y_labels_hum, s_detail_y_labels_pres};

    for (int i = 0; i < 5; i++) {
        for (int m = 0; m < 4; m++) {
            if (all_labels[m][i] == NULL) continue;

            bool show = false;
            if (g_selected_metric == -1) {
                show = (m == 0 || m == 1);
            } else {
                show = (g_selected_metric == m);
            }

            if (show) {
                lv_obj_clear_flag(all_labels[m][i], LV_OBJ_FLAG_HIDDEN);
                int y_pos = s_y_label_positions[i];
                if (g_selected_metric != -1) {
                    lv_obj_set_pos(all_labels[m][i], s_plot_left_x, y_pos);
                } else if (m == 1) {
                    lv_obj_set_pos(all_labels[m][i], s_plot_right_x, y_pos);
                }
            } else {
                lv_obj_add_flag(all_labels[m][i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void metric_card_click_cb(lv_event_t *e)
{
    int metric_idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (g_selected_metric == metric_idx) {
        g_selected_metric = -1;
    } else {
        g_selected_metric = metric_idx;
    }

    update_metric_selection();
}

// ============================================================================
// Navigation Callbacks
// ============================================================================

static void back_btn_event_cb(lv_event_t *e)
{
    (void)e;

    if (detail_history_get_state() == DETAIL_HISTORY_IN_PROGRESS) {
        detail_history_cancel_download();
    }

    detail_history_clear();

    g_selected_sensor = -1;
    g_selected_metric = -1;
    s_last_download_state = DETAIL_HISTORY_IDLE;
    lv_screen_load(g_main_screen);
}

static void loading_cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Download cancel requested");
    detail_history_cancel_download();
}

static void settings_btn_event_cb(lv_event_t *e)
{
    (void)e;
    if (g_selected_sensor < 0 || g_selected_sensor >= SENSOR_COUNT) {
        return;
    }
    ui_settings_screen_show();
}

// ============================================================================
// Screen Creation
// ============================================================================

void ui_detail_screen_create(void)
{
    g_detail_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_detail_screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(g_detail_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_detail_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_label_create(g_detail_screen);
    lv_label_set_text(back_btn, "< Back");
    lv_obj_set_style_text_color(back_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(back_btn, DETAIL_BACK_BTN_X, DETAIL_BACK_BTN_Y);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *settings_btn = lv_label_create(g_detail_screen);
    lv_label_set_text(settings_btn, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(settings_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_font(settings_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(settings_btn, UI_DISPLAY_WIDTH - 32, DETAIL_BACK_BTN_Y);
    lv_obj_add_flag(settings_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(settings_btn, settings_btn_event_cb, LV_EVENT_CLICKED, NULL);

    s_detail_name_label = lv_label_create(g_detail_screen);
    lv_obj_set_style_text_color(s_detail_name_label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_name_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_name_label, "Sensor");
    lv_obj_align(s_detail_name_label, LV_ALIGN_TOP_MID, 0, DETAIL_TITLE_Y);

    // Metric cards
    int cards_y = DETAIL_CARDS_Y;
    int card_h = DETAIL_CARD_HEIGHT;
    int card_w = DETAIL_CARD_WIDTH;
    int gap = DETAIL_CARD_GAP;
    int total_w = (card_w * 4) + (gap * 3);
    int start_x = (UI_DISPLAY_WIDTH - total_w) / 2;

    s_detail_co2_card = create_metric_card(g_detail_screen, start_x, cards_y, card_w, card_h, COLOR_GOOD, "CO2");
    lv_obj_add_flag(s_detail_co2_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_co2_card, metric_card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    s_detail_co2_value = lv_label_create(s_detail_co2_card);
    lv_obj_set_style_text_color(s_detail_co2_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_co2_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_co2_value, "--- ppm");
    lv_obj_set_pos(s_detail_co2_value, DETAIL_CARD_VALUE_X, DETAIL_CARD_VALUE_Y);

    s_detail_temp_card = create_metric_card(g_detail_screen, start_x + card_w + gap, cards_y, card_w, card_h, COLOR_TEMP, "TEMP");
    lv_obj_add_flag(s_detail_temp_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_temp_card, metric_card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    s_detail_temp_value = lv_label_create(s_detail_temp_card);
    lv_obj_set_style_text_color(s_detail_temp_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_temp_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_temp_value, "--.- \xC2\xB0" "C");
    lv_obj_set_pos(s_detail_temp_value, DETAIL_CARD_VALUE_X, DETAIL_CARD_VALUE_Y);

    s_detail_hum_card = create_metric_card(g_detail_screen, start_x + 2 * (card_w + gap), cards_y, card_w, card_h, COLOR_HUMIDITY, "HUMIDITY");
    lv_obj_add_flag(s_detail_hum_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_hum_card, metric_card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    s_detail_hum_value = lv_label_create(s_detail_hum_card);
    lv_obj_set_style_text_color(s_detail_hum_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_hum_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_hum_value, "--.- %");
    lv_obj_set_pos(s_detail_hum_value, DETAIL_CARD_VALUE_X, DETAIL_CARD_VALUE_Y);

    s_detail_pres_card = create_metric_card(g_detail_screen, start_x + 3 * (card_w + gap), cards_y, card_w, card_h, COLOR_PRESSURE, "PRESSURE");
    lv_obj_add_flag(s_detail_pres_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_pres_card, metric_card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)3);
    s_detail_pres_value = lv_label_create(s_detail_pres_card);
    lv_obj_set_style_text_color(s_detail_pres_value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_detail_pres_value, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_detail_pres_value, "---- hPa");
    lv_obj_set_pos(s_detail_pres_value, DETAIL_CARD_VALUE_X, DETAIL_CARD_VALUE_Y);

    // Chart container
    int chart_container_top = DETAIL_CHART_TOP;
    int chart_container_h = UI_DISPLAY_HEIGHT - chart_container_top - DETAIL_CHART_MARGIN_BOTTOM;
    int chart_container_w = UI_DISPLAY_WIDTH - (DETAIL_CHART_MARGIN_X * 2);

    s_detail_chart_container = lv_obj_create(g_detail_screen);
    lv_obj_t *chart_container = s_detail_chart_container;
    lv_obj_set_pos(chart_container, DETAIL_CHART_MARGIN_X, chart_container_top);
    lv_obj_set_size(chart_container, chart_container_w, chart_container_h);
    lv_obj_set_style_bg_color(chart_container, lv_color_hex(COLOR_BG_CHART), 0);
    lv_obj_set_style_bg_opa(chart_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart_container, DETAIL_CHART_RADIUS, 0);
    lv_obj_set_style_border_width(chart_container, 0, 0);
    lv_obj_set_style_pad_all(chart_container, 0, 0);
    lv_obj_clear_flag(chart_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(chart_container, LV_LAYOUT_NONE, 0);

    int plot_top = PLOT_MARGIN_TOP;
    int plot_bottom = chart_container_h - PLOT_MARGIN_BOTTOM;
    int plot_left = PLOT_MARGIN_LEFT;
    int plot_right = chart_container_w - PLOT_MARGIN_RIGHT;
    s_plot_left_x = PLOT_Y_LABEL_X;

    s_detail_chart = lv_chart_create(chart_container);
    lv_obj_set_pos(s_detail_chart, plot_left, plot_top);
    lv_obj_set_size(s_detail_chart, plot_right - plot_left, plot_bottom - plot_top);
    lv_obj_set_style_bg_color(s_detail_chart, lv_color_hex(COLOR_BG_CHART), 0);
    lv_obj_set_style_bg_opa(s_detail_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_detail_chart, 0, 0);
    lv_obj_set_style_border_width(s_detail_chart, 0, 0);
    lv_obj_set_style_pad_all(s_detail_chart, 0, 0);
    lv_obj_set_style_line_width(s_detail_chart, CHART_LINE_WIDTH, LV_PART_ITEMS);
    lv_obj_set_style_width(s_detail_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_detail_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_detail_chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_line_opa(s_detail_chart, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_detail_chart, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_line_color(s_detail_chart, lv_color_hex(COLOR_GRID_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_detail_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_chart_set_type(s_detail_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_detail_chart, SENSOR_HISTORY_SIZE);
    lv_chart_set_div_line_count(s_detail_chart, CHART_DIV_LINES, CHART_DIV_LINES);
    lv_chart_set_axis_range(s_detail_chart, LV_CHART_AXIS_PRIMARY_Y, DETAIL_CHART_Y_MIN, DETAIL_CHART_Y_MAX);
    lv_obj_clear_flag(s_detail_chart, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_pres_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_PRESSURE), LV_CHART_AXIS_PRIMARY_Y);
    s_detail_hum_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_HUMIDITY), LV_CHART_AXIS_PRIMARY_Y);
    s_detail_temp_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_TEMP), LV_CHART_AXIS_PRIMARY_Y);
    s_detail_co2_series = lv_chart_add_series(s_detail_chart, lv_color_hex(COLOR_GOOD), LV_CHART_AXIS_PRIMARY_Y);

    // Initialize chart data
    for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
        s_detail_co2_data[j] = LV_CHART_POINT_NONE;
        s_detail_temp_data[j] = LV_CHART_POINT_NONE;
        s_detail_hum_data[j] = LV_CHART_POINT_NONE;
        s_detail_pres_data[j] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_values(s_detail_chart, s_detail_co2_series, s_detail_co2_data, SENSOR_HISTORY_SIZE);
    lv_chart_set_series_values(s_detail_chart, s_detail_temp_series, s_detail_temp_data, SENSOR_HISTORY_SIZE);
    lv_chart_set_series_values(s_detail_chart, s_detail_hum_series, s_detail_hum_data, SENSOR_HISTORY_SIZE);
    lv_chart_set_series_values(s_detail_chart, s_detail_pres_series, s_detail_pres_data, SENSOR_HISTORY_SIZE);

    // Y-axis labels
    int chart_h = plot_bottom - plot_top;
    int y_div = chart_h / 4;
    for (int i = 0; i < 5; i++) {
        s_y_label_positions[i] = plot_top + i * y_div - PLOT_Y_LABEL_OFFSET;
    }

    const char *y_labels[] = {"1.6k", "1.3k", "1k", "700", "400"};
    uint32_t co2_colors[] = {0xFF4757, 0xFF8C32, 0xFFD93D, 0x7ED321, 0x3FB950};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *y_lbl = lv_label_create(chart_container);
        lv_obj_set_style_text_color(y_lbl, lv_color_hex(co2_colors[i]), 0);
        lv_obj_set_style_text_font(y_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(y_lbl, y_labels[i]);
        lv_obj_set_pos(y_lbl, PLOT_Y_LABEL_X, plot_top + i * y_div - PLOT_Y_LABEL_OFFSET);
        s_detail_y_labels_co2[i] = y_lbl;
    }

    const char *y_temp_labels[] = {"30", "26", "22", "18", "15"};
    uint32_t temp_colors[] = {0xFF8C32, 0xFFD93D, 0x7ED321, 0x60CDE4, 0x29B6F6};
    int temp_label_x = chart_container_w - 24;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *yt_lbl = lv_label_create(chart_container);
        lv_obj_set_style_text_color(yt_lbl, lv_color_hex(temp_colors[i]), 0);
        lv_obj_set_style_text_font(yt_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(yt_lbl, y_temp_labels[i]);
        lv_obj_set_pos(yt_lbl, temp_label_x, s_y_label_positions[i]);
        s_detail_y_labels_temp[i] = yt_lbl;
    }

    const char *y_hum_labels[] = {"80%", "65%", "50%", "35%", "20%"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *yh_lbl = lv_label_create(chart_container);
        lv_obj_set_style_text_color(yh_lbl, lv_color_hex(COLOR_HUMIDITY), 0);
        lv_obj_set_style_text_font(yh_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(yh_lbl, y_hum_labels[i]);
        lv_obj_set_pos(yh_lbl, temp_label_x, plot_top + i * y_div - PLOT_Y_LABEL_OFFSET);
        s_detail_y_labels_hum[i] = yh_lbl;
        lv_obj_add_flag(yh_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    const char *y_pres_labels[] = {"1050", "1025", "1000", "975", "950"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *yp_lbl = lv_label_create(chart_container);
        lv_obj_set_style_text_color(yp_lbl, lv_color_hex(COLOR_PRESSURE), 0);
        lv_obj_set_style_text_font(yp_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(yp_lbl, y_pres_labels[i]);
        lv_obj_set_pos(yp_lbl, temp_label_x, plot_top + i * y_div - PLOT_Y_LABEL_OFFSET);
        s_detail_y_labels_pres[i] = yp_lbl;
        lv_obj_add_flag(yp_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    s_plot_right_x = temp_label_x;

    // X-axis labels
    int chart_w = plot_right - plot_left;
    s_detail_x_labels[0] = lv_label_create(chart_container);
    lv_obj_set_style_text_color(s_detail_x_labels[0], lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_detail_x_labels[0], &lv_font_montserrat_12, 0);
    lv_label_set_text(s_detail_x_labels[0], "-60m");
    lv_obj_set_pos(s_detail_x_labels[0], plot_left, plot_bottom + PLOT_X_LABEL_Y_OFFSET);

    s_detail_x_labels[1] = lv_label_create(chart_container);
    lv_obj_set_style_text_color(s_detail_x_labels[1], lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_detail_x_labels[1], &lv_font_montserrat_12, 0);
    lv_label_set_text(s_detail_x_labels[1], "-30m");
    lv_obj_set_pos(s_detail_x_labels[1], plot_left + chart_w / 2 - PLOT_X_LABEL_MID_OFFSET, plot_bottom + PLOT_X_LABEL_Y_OFFSET);

    s_detail_x_labels[2] = lv_label_create(chart_container);
    lv_obj_set_style_text_color(s_detail_x_labels[2], lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_detail_x_labels[2], &lv_font_montserrat_12, 0);
    lv_label_set_text(s_detail_x_labels[2], "now");
    lv_obj_set_pos(s_detail_x_labels[2], plot_right - PLOT_X_LABEL_END_OFFSET, plot_bottom + PLOT_X_LABEL_Y_OFFSET);

    // Loading overlay
    s_loading_overlay = lv_obj_create(g_detail_screen);
    lv_obj_set_size(s_loading_overlay, UI_DISPLAY_WIDTH, UI_DISPLAY_HEIGHT);
    lv_obj_set_pos(s_loading_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_loading_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_loading_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_loading_overlay, 0, 0);
    lv_obj_clear_flag(s_loading_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);

    s_loading_spinner = lv_spinner_create(s_loading_overlay);
    lv_obj_set_size(s_loading_spinner, 50, 50);
    lv_obj_align(s_loading_spinner, LV_ALIGN_CENTER, 0, -30);
    lv_spinner_set_anim_params(s_loading_spinner, 1000, 200);

    s_loading_progress_label = lv_label_create(s_loading_overlay);
    lv_obj_set_style_text_color(s_loading_progress_label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_loading_progress_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_loading_progress_label, "Syncing 0%");
    lv_obj_align(s_loading_progress_label, LV_ALIGN_CENTER, 0, 20);

    s_loading_cancel_btn = lv_btn_create(s_loading_overlay);
    lv_obj_set_size(s_loading_cancel_btn, 80, 32);
    lv_obj_align(s_loading_cancel_btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(s_loading_cancel_btn, lv_color_hex(COLOR_ALERT), 0);
    lv_obj_set_style_radius(s_loading_cancel_btn, 6, 0);
    lv_obj_add_event_cb(s_loading_cancel_btn, loading_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(s_loading_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_12, 0);
    lv_obj_center(cancel_label);
}

// ============================================================================
// Screen Update
// ============================================================================

void ui_detail_screen_update(int sensor_idx)
{
    if (g_detail_screen == NULL || sensor_idx < 0 || sensor_idx >= SENSOR_COUNT) {
        return;
    }

    sensor_data_t *sensor = sensor_data_get(sensor_idx);
    if (sensor == NULL) {
        return;
    }

    // Handle download state changes
    detail_history_state_t dl_state = detail_history_get_state();
    if (dl_state != s_last_download_state) {
        s_last_download_state = dl_state;

        if (dl_state == DETAIL_HISTORY_COMPLETE) {
            ESP_LOGI(TAG, "Extended history download complete");
            LOG_HEAP("download_complete");
            if (s_loading_overlay != NULL) {
                lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (dl_state == DETAIL_HISTORY_CANCELLED) {
            ESP_LOGI(TAG, "Extended history download cancelled");
            if (s_loading_overlay != NULL) {
                lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (dl_state == DETAIL_HISTORY_ERROR) {
            ESP_LOGW(TAG, "Extended history download failed");
            if (s_loading_overlay != NULL) {
                lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Update progress if downloading
    if (dl_state == DETAIL_HISTORY_IN_PROGRESS && s_loading_progress_label != NULL) {
        uint8_t progress = detail_history_get_progress();
        char progress_str[24];
        if (progress == 0) {
            snprintf(progress_str, sizeof(progress_str), "Preparing...");
        } else {
            snprintf(progress_str, sizeof(progress_str), "Syncing %d%%", progress);
        }
        lv_label_set_text(s_loading_progress_label, progress_str);
    }

    // Clear chart data
    for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
        s_detail_co2_data[j] = LV_CHART_POINT_NONE;
        s_detail_temp_data[j] = LV_CHART_POINT_NONE;
        s_detail_hum_data[j] = LV_CHART_POINT_NONE;
        s_detail_pres_data[j] = LV_CHART_POINT_NONE;
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

    lv_color_t col = ui_status_color(status);

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

    // Check for extended history data
    uint16_t detail_count = detail_history_get_count();
    bool use_extended = (dl_state == DETAIL_HISTORY_COMPLETE || dl_state == DETAIL_HISTORY_CANCELLED)
                        && detail_count > 0
                        && detail_history_get_sensor_idx() == sensor_idx;

    ESP_LOGD(TAG, "Update: dl_state=%d, detail_count=%u, sensor_idx_match=%d, use_extended=%d",
             dl_state, detail_count, detail_history_get_sensor_idx() == sensor_idx, use_extended);

    if (use_extended) {
        LOG_HEAP("use_extended_start");
        const int16_t *ext_co2 = detail_history_get_co2(NULL);
        const int16_t *ext_temp = detail_history_get_temp(NULL);
        const int16_t *ext_hum = detail_history_get_hum(NULL);
        const int16_t *ext_pres = detail_history_get_pres(NULL);

        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            uint16_t src_idx = (j * detail_count) / SENSOR_HISTORY_SIZE;
            if (src_idx >= detail_count) src_idx = detail_count - 1;

            s_detail_co2_data[j] = ext_co2[src_idx];

            int16_t t = ext_temp[src_idx];
            int32_t val;
            if (t <= 0 || t < TEMP_MIN_TENTHS - 50 || t > TEMP_MAX_TENTHS + 100) {
                val = (DETAIL_CHART_Y_MIN + DETAIL_CHART_Y_MAX) / 2;
            } else {
                val = DETAIL_CHART_Y_MIN + ((t - TEMP_MIN_TENTHS) * DETAIL_CHART_Y_RANGE / TEMP_RANGE_TENTHS);
            }
            if (val < DETAIL_CHART_Y_MIN) val = DETAIL_CHART_Y_MIN;
            if (val > DETAIL_CHART_Y_MAX) val = DETAIL_CHART_Y_MAX;
            s_detail_temp_data[j] = val;

            int16_t h = ext_hum[src_idx];
            if (h <= 0 || h > 100) {
                val = (DETAIL_CHART_Y_MIN + DETAIL_CHART_Y_MAX) / 2;
            } else {
                val = DETAIL_CHART_Y_MIN + ((h - HUM_MIN_PERCENT) * DETAIL_CHART_Y_RANGE / HUM_RANGE_PERCENT);
            }
            if (val < DETAIL_CHART_Y_MIN) val = DETAIL_CHART_Y_MIN;
            if (val > DETAIL_CHART_Y_MAX) val = DETAIL_CHART_Y_MAX;
            s_detail_hum_data[j] = val;

            int16_t p = ext_pres[src_idx];
            if (p <= 0 || p < PRES_MIN_HPA || p > PRES_MAX_HPA) {
                val = (DETAIL_CHART_Y_MIN + DETAIL_CHART_Y_MAX) / 2;
            } else {
                val = DETAIL_CHART_Y_MIN + ((p - PRES_MIN_HPA) * DETAIL_CHART_Y_RANGE / PRES_RANGE_HPA);
            }
            if (val < DETAIL_CHART_Y_MIN) val = DETAIL_CHART_Y_MIN;
            if (val > DETAIL_CHART_Y_MAX) val = DETAIL_CHART_Y_MAX;
            s_detail_pres_data[j] = val;
        }

        lv_chart_set_series_values(s_detail_chart, s_detail_co2_series, s_detail_co2_data, SENSOR_HISTORY_SIZE);
        lv_chart_set_series_color(s_detail_chart, s_detail_co2_series, col);
        lv_chart_set_series_values(s_detail_chart, s_detail_temp_series, s_detail_temp_data, SENSOR_HISTORY_SIZE);
        lv_chart_set_series_values(s_detail_chart, s_detail_hum_series, s_detail_hum_data, SENSOR_HISTORY_SIZE);
        lv_chart_set_series_values(s_detail_chart, s_detail_pres_series, s_detail_pres_data, SENSOR_HISTORY_SIZE);

    } else {
        // Use short buffer data
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

    update_metric_selection();

    // Update time labels
    uint16_t total_mins;
    if (use_extended) {
        total_mins = detail_history_get_total_minutes();
    } else {
        total_mins = sensor_data_get_total_minutes(sensor_idx);
    }

    if (total_mins > 0 && s_detail_x_labels[0] != NULL) {
        char label_buf[16];

        ui_format_time_label(total_mins, label_buf, sizeof(label_buf));
        lv_label_set_text(s_detail_x_labels[0], label_buf);

        ui_format_time_label(total_mins / 2, label_buf, sizeof(label_buf));
        lv_label_set_text(s_detail_x_labels[1], label_buf);

        lv_label_set_text(s_detail_x_labels[2], "now");
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void ui_detail_screen_show(int sensor_idx)
{
    ESP_LOGW(TAG, ">>> DETAIL SCREEN SHOW: sensor_idx=%d <<<", sensor_idx);
    LOG_HEAP("detail_show_start");

    if (sensor_idx < 0 || sensor_idx >= SENSOR_COUNT) {
        ESP_LOGW(TAG, "Invalid sensor index, ignoring");
        return;
    }

    g_selected_sensor = sensor_idx;
    s_last_download_state = DETAIL_HISTORY_IDLE;

    if (g_detail_screen == NULL) {
        LOG_HEAP("before_create");
        ui_detail_screen_create();
        LOG_HEAP("after_create");
    }

    // Check if history data is already available
    detail_history_state_t state = detail_history_get_state();
    uint16_t count = detail_history_get_count();
    uint8_t current_sensor = detail_history_get_sensor_idx();

    if (state == DETAIL_HISTORY_COMPLETE && count > 0 && current_sensor == (uint8_t)sensor_idx) {
        ESP_LOGI(TAG, "Using existing history data: %u records", count);
        s_last_download_state = DETAIL_HISTORY_COMPLETE;
        ui_detail_screen_update(sensor_idx);
        lv_screen_load(g_detail_screen);
        if (s_loading_overlay != NULL) {
            lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    ui_detail_screen_update(sensor_idx);
    lv_screen_load(g_detail_screen);

    // Show loading overlay and start download
    if (s_loading_overlay != NULL) {
        lv_label_set_text(s_loading_progress_label, "Syncing 0%");
        lv_obj_clear_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    if (detail_history_start_download(sensor_idx)) {
        ESP_LOGW(TAG, ">>> HISTORY DOWNLOAD STARTED for sensor %d <<<", sensor_idx);
    } else {
        ESP_LOGE(TAG, ">>> FAILED TO START HISTORY DOWNLOAD for sensor %d <<<", sensor_idx);
        if (s_loading_overlay != NULL) {
            lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_detail_screen_reset_state(void)
{
    s_last_download_state = DETAIL_HISTORY_IDLE;
}

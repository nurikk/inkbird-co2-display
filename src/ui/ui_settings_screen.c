/**
 * @file ui_settings_screen.c
 * @brief Settings screen for device configuration
 *
 * Displays and allows modification of sensor settings including
 * CO2 thresholds, alarm settings, and calibration offsets.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"

#include "lvgl.h"

#include "inkbird_ble.h"
#include "ui_internal.h"

static const char *TAG = "ui_settings";

// ============================================================================
// Private State - Settings UI Elements
// ============================================================================

static lv_obj_t *s_settings_mode_label = NULL;
static lv_obj_t *s_settings_auto_cal_sw = NULL;
static lv_obj_t *s_settings_custom_mode_sw = NULL;
static lv_obj_t *s_settings_normal_high_label = NULL;
static lv_obj_t *s_settings_normal_low_label = NULL;
static lv_obj_t *s_settings_plant_high_label = NULL;
static lv_obj_t *s_settings_plant_low_label = NULL;
static lv_obj_t *s_settings_alarm_sw = NULL;
static lv_obj_t *s_settings_alarm_value_label = NULL;
static lv_obj_t *s_settings_co2_offset_label = NULL;
static lv_obj_t *s_settings_temp_offset_label = NULL;
static lv_obj_t *s_settings_hum_offset_label = NULL;
static lv_obj_t *s_settings_unit_label = NULL;

// ============================================================================
// Helper Functions
// ============================================================================

static lv_obj_t *create_settings_row(lv_obj_t *parent, int x, int y, const char *label_text, lv_obj_t **out_value_label)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_text(label, label_text);
    lv_obj_set_pos(label, x, y);

    lv_obj_t *value = lv_label_create(parent);
    lv_obj_set_style_text_color(value, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_12, 0);
    lv_label_set_text(value, "--");
    lv_obj_set_pos(value, x + SETTINGS_VALUE_X, y);

    if (out_value_label) {
        *out_value_label = value;
    }
    return label;
}

static lv_obj_t *create_settings_switch_row(lv_obj_t *parent, int x, int y, const char *label_text, lv_obj_t **out_switch, lv_event_cb_t cb)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_text(label, label_text);
    lv_obj_set_pos(label, x, y);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 40, 20);
    lv_obj_set_pos(sw, x + SETTINGS_VALUE_X, y - 2);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_TILE_BG), 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_GOOD), LV_PART_INDICATOR | LV_STATE_CHECKED);

    if (cb != NULL) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    if (out_switch) {
        *out_switch = sw;
    }
    return label;
}

static lv_obj_t *create_settings_header(lv_obj_t *parent, int x, int y, const char *text, uint32_t color)
{
    lv_obj_t *header = lv_label_create(parent);
    lv_obj_set_style_text_color(header, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_12, 0);
    lv_label_set_text(header, text);
    lv_obj_set_pos(header, x, y);
    return header;
}

// ============================================================================
// Switch Event Handlers
// ============================================================================

static void auto_cal_switch_cb(lv_event_t *e)
{
    if (g_selected_sensor < 0) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    inkbird_device_settings_t settings = inkbird_ble_get_settings(g_selected_sensor);
    ESP_LOGI(TAG, "Setting auto calibration to %d for sensor %d", checked, g_selected_sensor);
    inkbird_ble_set_co2_mode(g_selected_sensor,
                             settings.co2_settings.display_mode,
                             settings.co2_settings.use_custom,
                             checked);
}

static void custom_mode_switch_cb(lv_event_t *e)
{
    if (g_selected_sensor < 0) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    inkbird_device_settings_t settings = inkbird_ble_get_settings(g_selected_sensor);
    ESP_LOGI(TAG, "Setting custom/plant mode to %d for sensor %d", checked, g_selected_sensor);
    inkbird_ble_set_co2_mode(g_selected_sensor,
                             settings.co2_settings.display_mode,
                             checked,
                             settings.co2_settings.auto_calibration);
}

static void alarm_switch_cb(lv_event_t *e)
{
    if (g_selected_sensor < 0) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    inkbird_device_settings_t settings = inkbird_ble_get_settings(g_selected_sensor);
    ESP_LOGI(TAG, "Setting alarm enabled to %d for sensor %d", checked, g_selected_sensor);
    inkbird_ble_set_alarm(g_selected_sensor,
                          checked,
                          settings.alarm.alarm_mode,
                          settings.alarm.alarm_value);
}

// ============================================================================
// Navigation Callbacks
// ============================================================================

static void settings_back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_detail_screen != NULL) {
        lv_screen_load(g_detail_screen);
    }
}

// ============================================================================
// Screen Creation
// ============================================================================

void ui_settings_screen_create(void)
{
    g_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_settings_screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(g_settings_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_settings_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Back button
    lv_obj_t *back_btn = lv_label_create(g_settings_screen);
    lv_label_set_text(back_btn, "< Back");
    lv_obj_set_style_text_color(back_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(back_btn, 16, 8);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, settings_back_btn_cb, LV_EVENT_CLICKED, NULL);

    // Title
    lv_obj_t *title = lv_label_create(g_settings_screen);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Device Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    int row_h = 20;
    int y1 = 32;  // Left column Y start
    int y2 = 32;  // Right column Y start

    // ==================== LEFT COLUMN ====================
    // === CO2 Settings Section ===
    create_settings_header(g_settings_screen, SETTINGS_COL1_X, y1, "CO2 Settings", COLOR_GOOD);
    y1 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL1_X, y1, "Display Mode:", &s_settings_mode_label);
    y1 += row_h;

    create_settings_switch_row(g_settings_screen, SETTINGS_COL1_X, y1, "Auto Cal:", &s_settings_auto_cal_sw, auto_cal_switch_cb);
    y1 += row_h;

    create_settings_switch_row(g_settings_screen, SETTINGS_COL1_X, y1, "Plant Mode:", &s_settings_custom_mode_sw, custom_mode_switch_cb);
    y1 += row_h + 6;

    // === Thresholds Section ===
    create_settings_header(g_settings_screen, SETTINGS_COL1_X, y1, "CO2 Thresholds", COLOR_MODERATE);
    y1 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL1_X, y1, "Normal High:", &s_settings_normal_high_label);
    y1 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL1_X, y1, "Normal Low:", &s_settings_normal_low_label);
    y1 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL1_X, y1, "Plant High:", &s_settings_plant_high_label);
    y1 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL1_X, y1, "Plant Low:", &s_settings_plant_low_label);

    // ==================== RIGHT COLUMN ====================
    // === Alarm Section ===
    create_settings_header(g_settings_screen, SETTINGS_COL2_X, y2, "Alarm Settings", COLOR_ALERT);
    y2 += row_h;

    create_settings_switch_row(g_settings_screen, SETTINGS_COL2_X, y2, "Enabled:", &s_settings_alarm_sw, alarm_switch_cb);
    y2 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL2_X, y2, "Threshold:", &s_settings_alarm_value_label);
    y2 += row_h + 6;

    // === Calibration Section ===
    create_settings_header(g_settings_screen, SETTINGS_COL2_X, y2, "Calibration", COLOR_TEMP);
    y2 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL2_X, y2, "CO2 Offset:", &s_settings_co2_offset_label);
    y2 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL2_X, y2, "Temp Offset:", &s_settings_temp_offset_label);
    y2 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL2_X, y2, "Hum Offset:", &s_settings_hum_offset_label);
    y2 += row_h;

    create_settings_row(g_settings_screen, SETTINGS_COL2_X, y2, "Temp Unit:", &s_settings_unit_label);
}

// ============================================================================
// Screen Update
// ============================================================================

void ui_settings_screen_update(void)
{
    if (g_settings_screen == NULL || g_selected_sensor < 0) {
        return;
    }

    inkbird_device_settings_t settings = inkbird_ble_get_settings(g_selected_sensor);

    // CO2 Settings
    if (s_settings_mode_label) {
        char mode_str[32];
        if (settings.co2_settings.valid) {
            snprintf(mode_str, sizeof(mode_str), "Mode: %d", settings.co2_settings.display_mode);
        } else {
            snprintf(mode_str, sizeof(mode_str), "Mode: --");
        }
        lv_label_set_text(s_settings_mode_label, mode_str);
    }

    if (s_settings_auto_cal_sw && settings.co2_settings.valid) {
        if (settings.co2_settings.auto_calibration) {
            lv_obj_add_state(s_settings_auto_cal_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_settings_auto_cal_sw, LV_STATE_CHECKED);
        }
    }

    if (s_settings_custom_mode_sw && settings.co2_settings.valid) {
        if (settings.co2_settings.use_custom) {
            lv_obj_add_state(s_settings_custom_mode_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_settings_custom_mode_sw, LV_STATE_CHECKED);
        }
    }

    // Thresholds
    if (s_settings_normal_high_label) {
        char str[24];
        if (settings.thresholds.thresholds_valid) {
            snprintf(str, sizeof(str), "%u ppm", settings.thresholds.normal_high_ppm);
        } else {
            snprintf(str, sizeof(str), "-- ppm");
        }
        lv_label_set_text(s_settings_normal_high_label, str);
    }

    if (s_settings_normal_low_label) {
        char str[24];
        if (settings.thresholds.thresholds_valid) {
            snprintf(str, sizeof(str), "%u ppm", settings.thresholds.normal_low_ppm);
        } else {
            snprintf(str, sizeof(str), "-- ppm");
        }
        lv_label_set_text(s_settings_normal_low_label, str);
    }

    if (s_settings_plant_high_label) {
        char str[24];
        if (settings.thresholds.thresholds_valid) {
            snprintf(str, sizeof(str), "%u ppm", settings.thresholds.plant_high_ppm);
        } else {
            snprintf(str, sizeof(str), "-- ppm");
        }
        lv_label_set_text(s_settings_plant_high_label, str);
    }

    if (s_settings_plant_low_label) {
        char str[24];
        if (settings.thresholds.thresholds_valid) {
            snprintf(str, sizeof(str), "%u ppm", settings.thresholds.plant_low_ppm);
        } else {
            snprintf(str, sizeof(str), "-- ppm");
        }
        lv_label_set_text(s_settings_plant_low_label, str);
    }

    // Alarm
    if (s_settings_alarm_sw && settings.alarm.valid) {
        if (settings.alarm.enabled) {
            lv_obj_add_state(s_settings_alarm_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_settings_alarm_sw, LV_STATE_CHECKED);
        }
    }

    if (s_settings_alarm_value_label) {
        char str[24];
        if (settings.alarm.valid) {
            snprintf(str, sizeof(str), "%u ppm", settings.alarm.alarm_value);
        } else {
            snprintf(str, sizeof(str), "-- ppm");
        }
        lv_label_set_text(s_settings_alarm_value_label, str);
    }

    // Calibration
    if (s_settings_co2_offset_label) {
        char str[24];
        if (settings.calibration.valid) {
            snprintf(str, sizeof(str), "%+d ppm", settings.calibration.co2_offset);
        } else {
            snprintf(str, sizeof(str), "-- ppm");
        }
        lv_label_set_text(s_settings_co2_offset_label, str);
    }

    if (s_settings_temp_offset_label) {
        char str[24];
        if (settings.calibration.valid) {
            int whole = settings.calibration.temp_offset / 10;
            int frac = settings.calibration.temp_offset % 10;
            if (frac < 0) frac = -frac;
            snprintf(str, sizeof(str), "%+d.%d", whole, frac);
        } else {
            snprintf(str, sizeof(str), "--.-");
        }
        lv_label_set_text(s_settings_temp_offset_label, str);
    }

    if (s_settings_hum_offset_label) {
        char str[24];
        if (settings.calibration.valid) {
            int whole = settings.calibration.hum_offset / 10;
            int frac = settings.calibration.hum_offset % 10;
            if (frac < 0) frac = -frac;
            snprintf(str, sizeof(str), "%+d.%d%%", whole, frac);
        } else {
            snprintf(str, sizeof(str), "--.- %%");
        }
        lv_label_set_text(s_settings_hum_offset_label, str);
    }

    if (s_settings_unit_label) {
        if (settings.calibration.valid) {
            lv_label_set_text(s_settings_unit_label, settings.calibration.use_fahrenheit ? "Fahrenheit" : "Celsius");
        } else {
            lv_label_set_text(s_settings_unit_label, "--");
        }
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void ui_settings_screen_show(void)
{
    if (g_selected_sensor < 0 || g_selected_sensor >= SENSOR_COUNT) {
        return;
    }

    if (g_settings_screen == NULL) {
        ui_settings_screen_create();
    }

    ui_settings_screen_update();
    lv_screen_load(g_settings_screen);
    ESP_LOGI(TAG, "Opening settings for sensor %d", g_selected_sensor);
}

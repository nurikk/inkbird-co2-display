/**
 * @file ui_internal.h
 * @brief Internal shared definitions for UI modules
 *
 * Contains common types, color constants, layout definitions, and
 * internal function declarations shared across all UI screen modules.
 */

#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sensor_data.h"

// ============================================================================
// Display Configuration
// ============================================================================

#ifdef UI_WIDTH
#define UI_DISPLAY_WIDTH    UI_WIDTH
#else
#define UI_DISPLAY_WIDTH    480
#endif

#ifdef UI_HEIGHT
#define UI_DISPLAY_HEIGHT   UI_HEIGHT
#else
#define UI_DISPLAY_HEIGHT   320
#endif

// ============================================================================
// Scaling System - Scale layout values based on display width
// ============================================================================

// Reference design width (original ESP32 board with 3.2" display)
#define UI_REF_WIDTH        480

// Scale factor as integer percentage (e.g., 67 for 320/480)
#define UI_SCALE_PERCENT    ((UI_DISPLAY_WIDTH * 100) / UI_REF_WIDTH)

// Scale a value from reference design to current display
// Uses integer math: (val * scale%) / 100, with minimum of 1
#define UI_SCALE(val)       (((val) * UI_SCALE_PERCENT + 50) / 100)

// Font selection based on display width
// Small displays (< 400px): use smaller fonts
// Large displays (>= 400px): use larger fonts
#if UI_DISPLAY_WIDTH < 400
#define UI_FONT_CO2         &lv_font_montserrat_28
#define UI_FONT_LABEL       &lv_font_montserrat_12
#define UI_FONT_SMALL       &lv_font_montserrat_12
#else
#define UI_FONT_CO2         &lv_font_montserrat_48
#define UI_FONT_LABEL       &lv_font_montserrat_14
#define UI_FONT_SMALL       &lv_font_montserrat_12
#endif

// ============================================================================
// Layout Constants - Main Screen
// ============================================================================

#define GRID_COLS           2
#define GRID_ROWS           2
#define GRID_GAP            UI_SCALE(6)
#define TILE_PAD            UI_SCALE(12)
#define TILE_BORDER_WIDTH   2
#define TILE_BORDER_RADIUS  UI_SCALE(12)
#define CHART_HEIGHT        UI_SCALE(42)
#define HEADER_HEIGHT       UI_SCALE(20)
#define CHART_MIN_PPM       400
#define CHART_MAX_PPM       2000

#define TILE_CHART_PAD          UI_SCALE(4)
#define TILE_CHART_RADIUS       UI_SCALE(6)
#define TILE_CHART_BG           0x0D1B2A
#define TILE_CHART_GRID_COLOR   0x2A3F5F
#define TILE_UNIT_SPACING       UI_SCALE(6)
#define TILE_STATUS_PAD_H       UI_SCALE(6)
#define TILE_STATUS_PAD_V       UI_SCALE(2)
#define TILE_STATUS_RADIUS      UI_SCALE(4)
#define TILE_STATUS_Y_OFFSET    UI_SCALE(2)
#define TILE_CO2_Y_OFFSET       UI_SCALE(15)
#define TILE_NAME_WIDTH_MARGIN  UI_SCALE(50)

// ============================================================================
// Layout Constants - Detail Screen
// ============================================================================

#define DETAIL_PAD              UI_SCALE(10)
#define DETAIL_BACK_BTN_X       UI_SCALE(16)
#define DETAIL_BACK_BTN_Y       UI_SCALE(10)
#define DETAIL_TITLE_Y          UI_SCALE(8)
#define DETAIL_CARDS_Y          UI_SCALE(36)
#define DETAIL_CARD_HEIGHT      UI_SCALE(52)
#define DETAIL_CARD_WIDTH       UI_SCALE(108)
#define DETAIL_CARD_GAP         UI_SCALE(8)
#define DETAIL_CARD_RADIUS      UI_SCALE(6)
#define DETAIL_CARD_VALUE_X     UI_SCALE(6)
#define DETAIL_CARD_VALUE_Y     UI_SCALE(12)
#define DETAIL_CARD_LABEL_X     UI_SCALE(6)
#define DETAIL_CARD_LABEL_Y_OFFSET  UI_SCALE(18)
#define DETAIL_CARD_BAR_X       UI_SCALE(4)
#define DETAIL_CARD_BAR_Y       UI_SCALE(3)
#define DETAIL_CARD_BAR_MARGIN  UI_SCALE(8)
#define DETAIL_CARD_BAR_HEIGHT  UI_SCALE(3)
#define DETAIL_CARD_BAR_RADIUS  1

#define DETAIL_CHART_TOP        UI_SCALE(92)
#define DETAIL_CHART_MARGIN_X   UI_SCALE(12)
#define DETAIL_CHART_MARGIN_BOTTOM  UI_SCALE(8)
#define DETAIL_CHART_RADIUS     UI_SCALE(8)

#define PLOT_MARGIN_TOP         UI_SCALE(8)
#define PLOT_MARGIN_BOTTOM      UI_SCALE(20)
#define PLOT_MARGIN_LEFT        UI_SCALE(32)
#define PLOT_MARGIN_RIGHT       UI_SCALE(28)
#define PLOT_Y_LABEL_X          UI_SCALE(2)
#define PLOT_Y_LABEL_OFFSET     UI_SCALE(6)
#define PLOT_Y_LABEL_RIGHT_OFFSET   UI_SCALE(4)
#define PLOT_X_LABEL_Y_OFFSET   UI_SCALE(4)
#define PLOT_X_LABEL_MID_OFFSET UI_SCALE(15)
#define PLOT_X_LABEL_END_OFFSET UI_SCALE(24)
#define CHART_LINE_WIDTH        2
#define CHART_DIV_LINES         4

// ============================================================================
// Layout Constants - Settings Screen
// ============================================================================

#define SETTINGS_COL1_X     UI_SCALE(12)
#define SETTINGS_COL2_X     UI_SCALE(248)
#define SETTINGS_LABEL_W    UI_SCALE(100)
#define SETTINGS_VALUE_X    UI_SCALE(112)

// ============================================================================
// Chart Data Range Constants
// ============================================================================

#define DETAIL_CHART_Y_MIN      400
#define DETAIL_CHART_Y_MAX      1600
#define DETAIL_CHART_Y_RANGE    (DETAIL_CHART_Y_MAX - DETAIL_CHART_Y_MIN)

#define TEMP_MIN_TENTHS         150
#define TEMP_MAX_TENTHS         300
#define TEMP_RANGE_TENTHS       (TEMP_MAX_TENTHS - TEMP_MIN_TENTHS)

#define HUM_MIN_PERCENT         20
#define HUM_MAX_PERCENT         80
#define HUM_RANGE_PERCENT       (HUM_MAX_PERCENT - HUM_MIN_PERCENT)

#define PRES_MIN_HPA            950
#define PRES_MAX_HPA            1050
#define PRES_RANGE_HPA          (PRES_MAX_HPA - PRES_MIN_HPA)

#define CO2_MIN_PPM             400
#define CO2_MAX_PPM             1600

// ============================================================================
// Timing Constants
// ============================================================================

#define LVGL_TICK_PERIOD_MS     5
#define LVGL_BUFFER_LINES       20

#define MINS_PER_HOUR           60
#define MINS_PER_DAY            1440

// ============================================================================
// Color Palette
// ============================================================================

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

// ============================================================================
// RGB565 Conversion Constants
// ============================================================================

#define RGB565_R_SHIFT      11
#define RGB565_G_SHIFT      5
#define RGB565_R_MASK       0x1F
#define RGB565_G_MASK       0x3F
#define RGB565_B_MASK       0x1F

// ============================================================================
// Touch Type Enumeration
// ============================================================================

typedef enum {
    TOUCH_TYPE_NONE,
    TOUCH_TYPE_GT911,
    TOUCH_TYPE_XPT2046
} touch_type_t;

// ============================================================================
// Shared State - Managed by ui_common.c
// ============================================================================

extern lv_display_t *g_ui_display;
extern lv_indev_t *g_ui_indev;
extern touch_type_t g_touch_type;

extern lv_obj_t *g_main_screen;
extern lv_obj_t *g_detail_screen;
extern lv_obj_t *g_settings_screen;

extern int g_selected_sensor;
extern int g_selected_metric;
extern bool g_ui_created;
extern bool g_loading_complete;

// ============================================================================
// Utility Functions - ui_common.c
// ============================================================================

/**
 * @brief Convert CO2 status to corresponding color
 */
lv_color_t ui_status_color(co2_status_t status);

/**
 * @brief Format time duration as human-readable label
 */
void ui_format_time_label(uint16_t minutes, char *buf, size_t buf_size);

// ============================================================================
// Screen Management Functions
// ============================================================================

// ui_loading.c
void ui_loading_create(void);
void ui_loading_set_status(const char *status);

// ui_main_screen.c
void ui_main_screen_create(void);
void ui_main_screen_update(void);

// ui_detail_screen.c
void ui_detail_screen_create(void);
void ui_detail_screen_update(int sensor_idx);
void ui_detail_screen_show(int sensor_idx);
void ui_detail_screen_reset_state(void);

// ui_settings_screen.c
void ui_settings_screen_create(void);
void ui_settings_screen_update(void);
void ui_settings_screen_show(void);

#endif // UI_INTERNAL_H

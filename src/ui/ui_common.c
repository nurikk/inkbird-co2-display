/**
 * @file ui_common.c
 * @brief LVGL initialization, display setup, and shared utilities
 *
 * Handles low-level display and touch controller initialization,
 * provides shared utility functions used by all UI screen modules.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "epd_driver.h"
#include "gt911_touch.h"
#include "xpt2046_touch.h"
#include "ui_internal.h"

static const char *TAG = "ui_common";

// ============================================================================
// Global State (shared across UI modules)
// ============================================================================

lv_display_t *g_ui_display = NULL;
lv_indev_t *g_ui_indev = NULL;
touch_type_t g_touch_type = TOUCH_TYPE_NONE;

lv_obj_t *g_main_screen = NULL;
lv_obj_t *g_detail_screen = NULL;
lv_obj_t *g_settings_screen = NULL;

int g_selected_sensor = -1;
int g_selected_metric = -1;
bool g_ui_created = false;
bool g_loading_complete = false;

// ============================================================================
// Private State
// ============================================================================

static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;
static uint8_t *s_rotate_buf = NULL;
static esp_timer_handle_t s_tick_timer = NULL;

// ============================================================================
// LVGL Tick Callback
// ============================================================================

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// ============================================================================
// RGB565 Color Swap for TFT Display
// ============================================================================

static void rgb565_to_bgr565_swap(uint16_t *buf, uint32_t px_count)
{
    for (uint32_t i = 0; i < px_count; i++) {
        uint16_t px = buf[i];
        uint16_t r = (px >> RGB565_R_SHIFT) & RGB565_R_MASK;
        uint16_t g = (px >> RGB565_G_SHIFT) & RGB565_G_MASK;
        uint16_t b = px & RGB565_B_MASK;
        uint16_t bgr = (b << RGB565_R_SHIFT) | (g << RGB565_G_SHIFT) | r;
        buf[i] = (bgr >> 8) | (bgr << 8);
    }
}

// ============================================================================
// LVGL Flush Callback
// ============================================================================

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t px_count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    rgb565_to_bgr565_swap((uint16_t *)px_map, px_count);
    epd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

// ============================================================================
// Touch Input Callback
// ============================================================================

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;

    if (g_touch_type == TOUCH_TYPE_GT911) {
        gt911_touch_data_t touch;
        if (gt911_read(&touch) && touch.pressed) {
            data->point.x = touch.x;
            data->point.y = touch.y;
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGI(TAG, "GT911 touch at (%d, %d)", touch.x, touch.y);
        }
    } else if (g_touch_type == TOUCH_TYPE_XPT2046) {
        xpt2046_touch_data_t touch;
        if (xpt2046_read(&touch) && touch.pressed) {
            data->point.x = touch.x;
            data->point.y = touch.y;
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGI(TAG, "XPT2046 touch at (%d, %d)", touch.x, touch.y);
        }
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

lv_color_t ui_status_color(co2_status_t status)
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

void ui_format_time_label(uint16_t minutes, char *buf, size_t buf_size)
{
    if (minutes == 0) {
        snprintf(buf, buf_size, "now");
    } else if (minutes < MINS_PER_HOUR) {
        snprintf(buf, buf_size, "-%um", minutes);
    } else if (minutes < MINS_PER_DAY) {
        uint16_t hours = minutes / MINS_PER_HOUR;
        uint16_t mins = minutes % MINS_PER_HOUR;
        if (mins == 0) {
            snprintf(buf, buf_size, "-%uh", hours);
        } else {
            snprintf(buf, buf_size, "-%uh %um", hours, mins);
        }
    } else {
        uint16_t days = minutes / MINS_PER_DAY;
        uint16_t hours = (minutes % MINS_PER_DAY) / MINS_PER_HOUR;
        if (hours == 0) {
            snprintf(buf, buf_size, "-%ud", days);
        } else {
            snprintf(buf, buf_size, "-%ud %uh", days, hours);
        }
    }
}

// ============================================================================
// Display Initialization
// ============================================================================

void ui_common_init(void)
{
    lv_init();

    g_ui_display = lv_display_create(UI_DISPLAY_WIDTH, UI_DISPLAY_HEIGHT);
    lv_display_set_default(g_ui_display);
    lv_display_set_color_format(g_ui_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(g_ui_display, lvgl_flush_cb);
    lv_display_set_rotation(g_ui_display, LV_DISPLAY_ROTATION_0);

    lv_theme_t *theme = lv_theme_default_init(
        g_ui_display,
        lv_color_hex(0x1E1E1E),
        lv_color_hex(0x2A2A2A),
        true,
        &lv_font_montserrat_14
    );
    lv_display_set_theme(g_ui_display, theme);

    size_t buf_pixels = UI_DISPLAY_WIDTH * LVGL_BUFFER_LINES;
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

    lv_display_set_buffers(g_ui_display, s_buf1, s_buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_rotate_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_rotate_buf == NULL) {
        s_rotate_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    if (s_rotate_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate rotation buffer");
        return;
    }

    // Initialize touch controller
    esp_err_t ret = gt911_init();
    if (ret == ESP_OK) {
        g_touch_type = TOUCH_TYPE_GT911;
        ESP_LOGI(TAG, "GT911 capacitive touch initialized");
    } else {
        ESP_LOGW(TAG, "GT911 init failed, trying XPT2046...");
        ret = xpt2046_init();
        if (ret == ESP_OK) {
            g_touch_type = TOUCH_TYPE_XPT2046;
            ESP_LOGI(TAG, "XPT2046 resistive touch initialized");
        } else {
            ESP_LOGW(TAG, "XPT2046 init failed, touch disabled");
        }
    }

    ESP_LOGI(TAG, ">>> Touch type after init: %d (0=none, 1=gt911, 2=xpt2046)", g_touch_type);

    if (g_touch_type != TOUCH_TYPE_NONE) {
        g_ui_indev = lv_indev_create();
        lv_indev_set_type(g_ui_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(g_ui_indev, touch_read_cb);
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

// ============================================================================
// Display Refresh
// ============================================================================

void ui_common_force_refresh(void)
{
    if (g_ui_display == NULL) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        lv_obj_invalidate(screen);
    }

    lv_refr_now(g_ui_display);
}

int ui_common_get_touch_type(void)
{
    return (int)g_touch_type;
}

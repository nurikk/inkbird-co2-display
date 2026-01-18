/**
 * @file ui_co2_display.c
 * @brief Public API coordinator for the UI subsystem
 *
 * Provides the public interface for UI initialization and updates.
 * Delegates to specialized screen modules for actual implementation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"

#include "lvgl.h"

#include "ui_co2_display.h"
#include "ui_internal.h"

static const char *TAG = "ui";

// ============================================================================
// External function declarations from submodules
// ============================================================================

// ui_common.c
extern void ui_common_init(void);
extern void ui_common_force_refresh(void);
extern int ui_common_get_touch_type(void);

// ui_loading.c
extern void ui_loading_apply_pending_status(void);
extern void ui_loading_complete(void);

// ============================================================================
// Public API Implementation
// ============================================================================

void ui_co2_display_init(void)
{
    ui_common_init();
}

void ui_co2_display_loading(void)
{
    ui_loading_create();
}

void ui_co2_display_set_status(const char *status)
{
    ui_loading_set_status(status);
}

void ui_co2_display_loading_complete(void)
{
    ui_loading_complete();
    ESP_LOGI(TAG, "Loading complete, main screen will be shown");
}

void ui_co2_display_update(void)
{
    // Apply pending status from other tasks
    ui_loading_apply_pending_status();

    // Don't create main UI until loading is complete
    if (!g_ui_created) {
        if (!g_loading_complete) {
            return;
        }
        ui_main_screen_create();
    }

    lv_obj_t *active_screen = lv_screen_active();

    if (active_screen == g_detail_screen && g_selected_sensor >= 0) {
        ui_detail_screen_update(g_selected_sensor);
        return;
    }

    ui_main_screen_update();
}

void ui_co2_display_force_refresh(void)
{
    ui_common_force_refresh();
}

int ui_co2_display_get_touch_type(void)
{
    return ui_common_get_touch_type();
}

void ui_co2_display_open_detail(int sensor_idx)
{
    ui_detail_screen_show(sensor_idx);
}

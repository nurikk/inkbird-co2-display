/**
 * @file ui_loading.c
 * @brief Loading/splash screen shown during application startup
 *
 * Displays application title and status messages while BLE and
 * sensor systems initialize in the background.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "lvgl.h"

#include "ui_internal.h"

static const char *TAG = "ui_loading";

// ============================================================================
// Private State
// ============================================================================

static lv_obj_t *s_loading_status_label = NULL;
static char s_pending_status[32] = "";

// ============================================================================
// Loading Screen
// ============================================================================

void ui_loading_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    g_ui_created = false;

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

void ui_loading_set_status(const char *status)
{
    // Thread-safe: just copy to buffer, display task will apply it
    if (status != NULL) {
        strncpy(s_pending_status, status, sizeof(s_pending_status) - 1);
        s_pending_status[sizeof(s_pending_status) - 1] = '\0';
    } else {
        s_pending_status[0] = '\0';
    }
}

void ui_loading_apply_pending_status(void)
{
    // Apply pending status text (thread-safe handoff from other tasks)
    if (s_pending_status[0] != '\0' && s_loading_status_label != NULL && !g_ui_created) {
        lv_label_set_text(s_loading_status_label, s_pending_status);
        lv_obj_align(s_loading_status_label, LV_ALIGN_CENTER, 0, 14);
        s_pending_status[0] = '\0';
    }
}

void ui_loading_complete(void)
{
    g_loading_complete = true;
    s_loading_status_label = NULL;
}

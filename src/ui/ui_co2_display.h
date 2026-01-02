/**
 * @file ui_co2_display.h
 * @brief Main CO2 display UI with 2x2 sensor grid
 */

#ifndef UI_CO2_DISPLAY_H
#define UI_CO2_DISPLAY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the CO2 display UI
 *
 * Creates the 2x2 grid layout with 4 sensor tiles.
 * Must be called after lv_init() and display driver initialization.
 */
void ui_co2_display_init(void);

/**
 * @brief Update all sensor tiles with current data
 *
 * Should be called periodically to refresh the display.
 */
void ui_co2_display_update(void);

/**
 * @brief Check if UI needs a display refresh
 *
 * @return true if display should be refreshed
 */
bool ui_co2_display_needs_refresh(void);

/**
 * @brief Mark display as refreshed
 */
void ui_co2_display_mark_refreshed(void);

#ifdef __cplusplus
}
#endif

#endif // UI_CO2_DISPLAY_H

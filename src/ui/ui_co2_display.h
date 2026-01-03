/**
 * @file ui_co2_display.h
 * @brief CO2 display UI using direct framebuffer rendering
 */

#ifndef UI_CO2_DISPLAY_H
#define UI_CO2_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the CO2 display UI
 */
void ui_co2_display_init(void);

/**
 * @brief Update the display with current sensor data
 */
void ui_co2_display_update(void);

#ifdef __cplusplus
}
#endif

#endif // UI_CO2_DISPLAY_H

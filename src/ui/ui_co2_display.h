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

/**
 * @brief Show loading screen during startup
 */
void ui_co2_display_loading(void);

/**
 * @brief Update the status text on the loading screen
 */
void ui_co2_display_set_status(const char *status);

void ui_co2_display_force_refresh(void);

/**
 * @brief Get the touch controller type
 * @return 0=none, 1=gt911, 2=xpt2046
 */
int ui_co2_display_get_touch_type(void);

/**
 * @brief Open the detail screen for a specific sensor
 *
 * Shows the detail screen and starts downloading historical data.
 * Used for programmatic navigation (e.g., auto-open after boot).
 *
 * @param sensor_idx Sensor index (0-3)
 */
void ui_co2_display_open_detail(int sensor_idx);

#ifdef __cplusplus
}
#endif

#endif // UI_CO2_DISPLAY_H

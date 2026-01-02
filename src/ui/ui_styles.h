/**
 * @file ui_styles.h
 * @brief UI styles optimized for monochrome e-paper display
 */

#ifndef UI_STYLES_H
#define UI_STYLES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize UI styles
 *
 * Creates and initializes all styles used in the CO2 display UI.
 * Must be called after lv_init().
 */
void ui_styles_init(void);

/**
 * @brief Get style for tile container
 */
lv_style_t *ui_style_tile(void);

/**
 * @brief Get style for large CO2 value text
 */
lv_style_t *ui_style_co2_value(void);

/**
 * @brief Get style for sensor name/title
 */
lv_style_t *ui_style_title(void);

/**
 * @brief Get style for temperature/humidity text
 */
lv_style_t *ui_style_secondary(void);

/**
 * @brief Get style for status indicator text
 */
lv_style_t *ui_style_status(void);

/**
 * @brief Get style for chart widget
 */
lv_style_t *ui_style_chart(void);

/**
 * @brief Get style for chart series (line)
 */
lv_style_t *ui_style_chart_series(void);

#ifdef __cplusplus
}
#endif

#endif // UI_STYLES_H

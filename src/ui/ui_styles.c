/**
 * @file ui_styles.c
 * @brief UI styles optimized for monochrome e-paper display
 *
 * All styles are designed for B/W e-paper:
 * - No gradients or transparency
 * - Solid borders and backgrounds
 * - High contrast text
 */

#include "ui_styles.h"

// Static style instances
static lv_style_t s_style_tile;
static lv_style_t s_style_co2_value;
static lv_style_t s_style_title;
static lv_style_t s_style_secondary;
static lv_style_t s_style_status;
static lv_style_t s_style_chart;
static lv_style_t s_style_chart_series;

void ui_styles_init(void)
{
    // Tile container style
    lv_style_init(&s_style_tile);
    lv_style_set_bg_color(&s_style_tile, lv_color_white());
    lv_style_set_bg_opa(&s_style_tile, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_tile, lv_color_black());
    lv_style_set_border_width(&s_style_tile, 1);
    lv_style_set_radius(&s_style_tile, 0);
    lv_style_set_pad_all(&s_style_tile, 4);
    lv_style_set_pad_gap(&s_style_tile, 2);
    
    // Large CO2 value style
    // Use the largest available font - will be configured via Kconfig
    lv_style_init(&s_style_co2_value);
    lv_style_set_text_color(&s_style_co2_value, lv_color_black());
#if LV_FONT_MONTSERRAT_28
    lv_style_set_text_font(&s_style_co2_value, &lv_font_montserrat_28);
#elif LV_FONT_MONTSERRAT_20
    lv_style_set_text_font(&s_style_co2_value, &lv_font_montserrat_20);
#else
    lv_style_set_text_font(&s_style_co2_value, &lv_font_montserrat_14);
#endif
    
    // Title/sensor name style
    lv_style_init(&s_style_title);
    lv_style_set_text_color(&s_style_title, lv_color_black());
    lv_style_set_text_font(&s_style_title, &lv_font_montserrat_14);
    
    // Secondary text (temp, humidity) style
    lv_style_init(&s_style_secondary);
    lv_style_set_text_color(&s_style_secondary, lv_color_black());
    lv_style_set_text_font(&s_style_secondary, &lv_font_montserrat_14);
    
    // Status indicator style
    lv_style_init(&s_style_status);
    lv_style_set_text_color(&s_style_status, lv_color_black());
    lv_style_set_text_font(&s_style_status, &lv_font_montserrat_14);
    
    // Chart container style
    lv_style_init(&s_style_chart);
    lv_style_set_bg_color(&s_style_chart, lv_color_white());
    lv_style_set_bg_opa(&s_style_chart, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_chart, lv_color_black());
    lv_style_set_border_width(&s_style_chart, 1);
    lv_style_set_radius(&s_style_chart, 0);
    lv_style_set_pad_all(&s_style_chart, 2);
    lv_style_set_line_width(&s_style_chart, 1);
    lv_style_set_line_color(&s_style_chart, lv_color_black());
    
    // Chart series (line) style
    lv_style_init(&s_style_chart_series);
    lv_style_set_line_width(&s_style_chart_series, 2);
    lv_style_set_line_color(&s_style_chart_series, lv_color_black());
    lv_style_set_size(&s_style_chart_series, 0, 0);  // No point markers
}

lv_style_t *ui_style_tile(void)
{
    return &s_style_tile;
}

lv_style_t *ui_style_co2_value(void)
{
    return &s_style_co2_value;
}

lv_style_t *ui_style_title(void)
{
    return &s_style_title;
}

lv_style_t *ui_style_secondary(void)
{
    return &s_style_secondary;
}

lv_style_t *ui_style_status(void)
{
    return &s_style_status;
}

lv_style_t *ui_style_chart(void)
{
    return &s_style_chart;
}

lv_style_t *ui_style_chart_series(void)
{
    return &s_style_chart_series;
}

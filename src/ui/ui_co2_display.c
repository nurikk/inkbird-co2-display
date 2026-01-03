/**
 * @file ui_co2_display.c
 * @brief CO2 display UI using direct framebuffer rendering
 *
 * Layout (400x300 pixels):
 * ┌───────────────────┬───────────────────┐
 * │     Sensor 1      │     Sensor 2      │
 * │     (198x148)     │     (198x148)     │
 * ├───────────────────┼───────────────────┤
 * │     Sensor 3      │     Sensor 4      │
 * │     (198x148)     │     (198x148)     │
 * └───────────────────┴───────────────────┘
 */

#include <stdio.h>
#include <string.h>

#include "ui_co2_display.h"
#include "sensor_data.h"
#include "epd_driver.h"
#include "gfx.h"

// Display dimensions
#define DISPLAY_WIDTH   EPD_WIDTH
#define DISPLAY_HEIGHT  EPD_HEIGHT

// Grid configuration
#define GRID_COLS       2
#define GRID_ROWS       2
#define GRID_GAP        2

// Tile dimensions
#define TILE_WIDTH      ((DISPLAY_WIDTH - GRID_GAP) / GRID_COLS)   // 199
#define TILE_HEIGHT     ((DISPLAY_HEIGHT - GRID_GAP) / GRID_ROWS)  // 149

// Padding inside tiles
#define TILE_PAD        4

/**
 * @brief Draw a mini chart of CO2 history
 */
static void draw_chart(int x, int y, int w, int h, const int16_t *data, int count)
{
    if (count < 2) return;
    
    // Find min/max for scaling
    int16_t min_val = data[0];
    int16_t max_val = data[0];
    for (int i = 1; i < count; i++) {
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }
    
    // Ensure some range
    if (max_val - min_val < 100) {
        int16_t mid = (max_val + min_val) / 2;
        min_val = mid - 50;
        max_val = mid + 50;
    }
    
    // Draw border
    gfx_draw_rect(x, y, w, h, true);
    
    // Draw data points as connected lines
    int chart_x = x + 1;
    int chart_y = y + 1;
    int chart_w = w - 2;
    int chart_h = h - 2;
    
    int prev_px = 0, prev_py = 0;
    for (int i = 0; i < count; i++) {
        int px = chart_x + (i * chart_w) / (count - 1);
        int py = chart_y + chart_h - 1 - ((data[i] - min_val) * (chart_h - 1)) / (max_val - min_val);
        
        if (i > 0) {
            gfx_draw_line(prev_px, prev_py, px, py, true);
        }
        prev_px = px;
        prev_py = py;
    }
}

/**
 * @brief Draw a single sensor tile
 */
static void draw_sensor_tile(int tile_x, int tile_y, int tile_w, int tile_h, int sensor_idx)
{
    sensor_data_t *sensor = sensor_data_get(sensor_idx);
    if (sensor == NULL) return;
    
    // Draw tile border
    gfx_draw_rect(tile_x, tile_y, tile_w, tile_h, true);
    
    int x = tile_x + TILE_PAD;
    int y = tile_y + TILE_PAD;
    int w = tile_w - TILE_PAD * 2;
    
    // Row 1: Sensor name and status
    gfx_draw_string(x, y, sensor->name, GFX_FONT_SMALL, true);
    
    // Status indicator on right
    co2_status_t status = sensor_data_get_co2_status(sensor->current.co2_ppm);
    const char *status_text = sensor_data_get_status_text(status);
    gfx_draw_string_right(tile_x + tile_w - TILE_PAD, y, status_text, GFX_FONT_SMALL, true);
    
    y += 12;
    
    // Row 2: CO2 value (large)
    if (sensor->connected) {
        char co2_str[16];
        snprintf(co2_str, sizeof(co2_str), "%d", sensor->current.co2_ppm);
        
        // Center the CO2 value
        int co2_width = gfx_string_width(co2_str, GFX_FONT_XLARGE);
        int unit_width = gfx_string_width(" ppm", GFX_FONT_SMALL);
        int total_width = co2_width + unit_width;
        int start_x = x + (w - total_width) / 2;
        
        gfx_draw_string(start_x, y, co2_str, GFX_FONT_XLARGE, true);
        gfx_draw_string(start_x + co2_width, y + 20, " ppm", GFX_FONT_SMALL, true);
    } else {
        gfx_draw_string(x + w/2 - 16, y + 10, "----", GFX_FONT_MEDIUM, true);
    }
    
    y += 38;
    
    // Row 3: Temperature and humidity
    if (sensor->connected) {
        char temp_str[24];
        char hum_str[24];
        
        // Temperature (value is in 0.1C units)
        int temp_whole = sensor->current.temperature / 10;
        int temp_frac = sensor->current.temperature % 10;
        if (temp_frac < 0) temp_frac = -temp_frac;
        snprintf(temp_str, sizeof(temp_str), "%d.%dC", temp_whole, temp_frac);
        
        // Humidity (value is in 0.1% units)  
        int hum_whole = sensor->current.humidity / 10;
        snprintf(hum_str, sizeof(hum_str), "%d%%", hum_whole);
        
        gfx_draw_string(x, y, temp_str, GFX_FONT_SMALL, true);
        gfx_draw_string_right(tile_x + tile_w - TILE_PAD, y, hum_str, GFX_FONT_SMALL, true);
    }
    
    y += 12;
    
    // Row 4: History chart or downloading message
    int chart_h = tile_h - (y - tile_y) - TILE_PAD;
    
    if (sensor->downloading) {
        // Show downloading message in chart area
        gfx_draw_rect(x, y, w, chart_h, true);
        gfx_draw_string(x + w/2 - 40, y + chart_h/2 - 4, "Downloading...", GFX_FONT_SMALL, true);
    } else {
        uint8_t history_count;
        const int16_t *history = sensor_data_get_co2_history(sensor_idx, &history_count);
        
        if (chart_h > 10 && history_count > 1) {
            draw_chart(x, y, w, chart_h, history, history_count);
        } else {
            // No history yet - draw empty chart area with message
            gfx_draw_rect(x, y, w, chart_h, true);
            gfx_draw_string(x + w/2 - 30, y + chart_h/2 - 4, "No history", GFX_FONT_SMALL, true);
        }
    }
}

void ui_co2_display_init(void)
{
    // Initialize graphics with framebuffer
    gfx_init(epd_get_framebuffer(), DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void ui_co2_display_update(void)
{
    // Clear screen to white
    gfx_fill(false);
    
    // Draw 2x2 grid of sensor tiles
    for (int i = 0; i < SENSOR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        
        int tile_x = col * (TILE_WIDTH + GRID_GAP);
        int tile_y = row * (TILE_HEIGHT + GRID_GAP);
        
        draw_sensor_tile(tile_x, tile_y, TILE_WIDTH, TILE_HEIGHT, i);
    }
}

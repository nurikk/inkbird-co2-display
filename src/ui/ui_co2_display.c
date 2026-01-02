/**
 * @file ui_co2_display.c
 * @brief Main CO2 display UI with 2x2 sensor grid
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

#include "ui_co2_display.h"
#include "ui_sensor_tile.h"
#include "ui_styles.h"
#include "sensor_data.h"
#include "epd_driver.h"

// Display dimensions
#define DISPLAY_WIDTH   400
#define DISPLAY_HEIGHT  300

// Grid configuration
#define GRID_COLS       2
#define GRID_ROWS       2
#define GRID_GAP        2

// Tile dimensions (accounting for gap)
#define TILE_WIDTH      ((DISPLAY_WIDTH - GRID_GAP) / GRID_COLS)
#define TILE_HEIGHT     ((DISPLAY_HEIGHT - GRID_GAP) / GRID_ROWS)

// Static storage for tiles
static ui_sensor_tile_t *s_tiles[SENSOR_COUNT];
static lv_obj_t *s_main_container = NULL;
static bool s_needs_refresh = false;

// Grid column and row descriptors
static int32_t s_col_dsc[] = { TILE_WIDTH, TILE_WIDTH, LV_GRID_TEMPLATE_LAST };
static int32_t s_row_dsc[] = { TILE_HEIGHT, TILE_HEIGHT, LV_GRID_TEMPLATE_LAST };

void ui_co2_display_init(void)
{
    // Initialize styles first
    ui_styles_init();
    
    // Get the active screen
    lv_obj_t *screen = lv_screen_active();
    
    // Set screen background to white and remove any default padding
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    
    // Create main container with grid layout
    s_main_container = lv_obj_create(screen);
    lv_obj_set_size(s_main_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_pos(s_main_container, 0, 0);
    lv_obj_set_style_bg_color(s_main_container, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_main_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_main_container, 0, 0);
    lv_obj_set_style_pad_all(s_main_container, 0, 0);
    lv_obj_set_style_pad_gap(s_main_container, GRID_GAP, 0);
    lv_obj_clear_flag(s_main_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Set up grid layout
    lv_obj_set_layout(s_main_container, LV_LAYOUT_GRID);
    lv_obj_set_style_grid_column_dsc_array(s_main_container, s_col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(s_main_container, s_row_dsc, 0);
    
    // Create sensor tiles in 2x2 grid
    // Sensor indices map to grid positions:
    // [0] [1]
    // [2] [3]
    for (int i = 0; i < SENSOR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        
        // Create tile
        s_tiles[i] = ui_sensor_tile_create(s_main_container, i);
        
        if (s_tiles[i] != NULL) {
            // Set tile size
            ui_sensor_tile_set_size(s_tiles[i], TILE_WIDTH, TILE_HEIGHT);
            
            // Position in grid
            lv_obj_set_grid_cell(s_tiles[i]->container, 
                                 LV_GRID_ALIGN_STRETCH, col, 1,
                                 LV_GRID_ALIGN_STRETCH, row, 1);
        }
    }
    
    s_needs_refresh = true;
}

void ui_co2_display_update(void)
{
    // Update all sensor tiles
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (s_tiles[i] != NULL) {
            ui_sensor_tile_update(s_tiles[i]);
        }
    }
    
    s_needs_refresh = true;
}

bool ui_co2_display_needs_refresh(void)
{
    return s_needs_refresh;
}

void ui_co2_display_mark_refreshed(void)
{
    s_needs_refresh = false;
}

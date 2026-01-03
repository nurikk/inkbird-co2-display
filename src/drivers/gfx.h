/**
 * @file gfx.h
 * @brief Simple graphics library for e-paper display
 */

#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Font sizes
typedef enum {
    GFX_FONT_SMALL = 1,   // 8x8, scale 1x
    GFX_FONT_MEDIUM = 2,  // 8x8, scale 2x (16x16)
    GFX_FONT_LARGE = 3,   // 8x8, scale 3x (24x24)
    GFX_FONT_XLARGE = 4,  // 8x8, scale 4x (32x32)
} gfx_font_size_t;

/**
 * @brief Initialize graphics library
 * @param framebuffer Pointer to display framebuffer
 * @param width Display width in pixels
 * @param height Display height in pixels
 */
void gfx_init(uint8_t *framebuffer, int width, int height);

/**
 * @brief Fill entire screen with color
 * @param black true for black, false for white
 */
void gfx_fill(bool black);

/**
 * @brief Set a single pixel
 */
void gfx_set_pixel(int x, int y, bool black);

/**
 * @brief Draw a line
 */
void gfx_draw_line(int x0, int y0, int x1, int y1, bool black);

/**
 * @brief Draw a rectangle (outline)
 */
void gfx_draw_rect(int x, int y, int w, int h, bool black);

/**
 * @brief Fill a rectangle
 */
void gfx_fill_rect(int x, int y, int w, int h, bool black);

/**
 * @brief Draw a horizontal line (fast)
 */
void gfx_draw_hline(int x, int y, int w, bool black);

/**
 * @brief Draw a vertical line (fast)
 */
void gfx_draw_vline(int x, int y, int h, bool black);

/**
 * @brief Draw a single character
 * @param x X position
 * @param y Y position
 * @param c Character to draw
 * @param size Font size (scale factor)
 * @param black true for black text, false for white text
 */
void gfx_draw_char(int x, int y, char c, gfx_font_size_t size, bool black);

/**
 * @brief Draw a string
 * @param x X position
 * @param y Y position
 * @param str String to draw
 * @param size Font size
 * @param black true for black text
 */
void gfx_draw_string(int x, int y, const char *str, gfx_font_size_t size, bool black);

/**
 * @brief Get width of a string in pixels
 */
int gfx_string_width(const char *str, gfx_font_size_t size);

/**
 * @brief Get height of font in pixels
 */
int gfx_font_height(gfx_font_size_t size);

/**
 * @brief Draw string centered horizontally
 */
void gfx_draw_string_centered(int y, const char *str, gfx_font_size_t size, bool black);

/**
 * @brief Draw string right-aligned
 */
void gfx_draw_string_right(int x_right, int y, const char *str, gfx_font_size_t size, bool black);

/**
 * @brief Draw integer number
 */
void gfx_draw_int(int x, int y, int value, gfx_font_size_t size, bool black);

/**
 * @brief Draw integer right-aligned
 */
void gfx_draw_int_right(int x_right, int y, int value, gfx_font_size_t size, bool black);

#ifdef __cplusplus
}
#endif

#endif // GFX_H

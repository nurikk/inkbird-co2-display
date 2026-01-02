/**
 * @file epd_driver.h
 * @brief E-Paper display driver for LVGL integration
 *
 * Waveshare 4.2" B/W E-Paper V1 (400x300) driver adapted for LVGL.
 */

#ifndef EPD_DRIVER_H
#define EPD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Display dimensions
#define EPD_WIDTH   400
#define EPD_HEIGHT  300

// Pin configuration for ESP32-C3
#define EPD_PIN_MOSI    7
#define EPD_PIN_SCK     6
#define EPD_PIN_CS      10
#define EPD_PIN_DC      1
#define EPD_PIN_RST     0
#define EPD_PIN_BUSY    3

/**
 * @brief Initialize the e-paper display hardware
 *
 * Sets up GPIO, SPI, and performs display initialization sequence.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t epd_init(void);

/**
 * @brief Initialize LVGL display driver
 *
 * Creates LVGL display, sets up buffers, and registers flush callback.
 * Must be called after epd_init() and lv_init().
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t epd_lvgl_init(void);

/**
 * @brief Trigger a full display refresh
 *
 * Sends the current framebuffer to the display and performs a full refresh.
 * This is a blocking operation that takes ~2-4 seconds.
 */
void epd_refresh(void);

/**
 * @brief Put the display into deep sleep mode
 *
 * Reduces power consumption when display updates are not needed.
 */
void epd_sleep(void);

/**
 * @brief Wake the display from deep sleep
 *
 * Must be called before any display operations after epd_sleep().
 */
void epd_wake(void);

/**
 * @brief Check if the display is currently busy
 *
 * @return true if display is busy refreshing, false if idle
 */
bool epd_is_busy(void);

/**
 * @brief Clear the display to white
 */
void epd_clear(void);

#ifdef __cplusplus
}
#endif

#endif // EPD_DRIVER_H

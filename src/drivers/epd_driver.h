/**
 * @file epd_driver.h
 * @brief E-Paper display driver (no LVGL)
 *
 * Waveshare 4.2" B/W E-Paper V1 (400x300) driver.
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
 * @return ESP_OK on success
 */
esp_err_t epd_init(void);

/**
 * @brief Trigger a full display refresh
 */
void epd_refresh(void);

/**
 * @brief Clear the display to white
 */
void epd_clear(void);

/**
 * @brief Put the display into deep sleep mode
 */
void epd_sleep(void);

/**
 * @brief Wake the display from deep sleep
 */
void epd_wake(void);

/**
 * @brief Check if the display is currently busy
 * @return true if busy, false if idle
 */
bool epd_is_busy(void);

/**
 * @brief Get pointer to framebuffer for direct drawing
 * @return Pointer to framebuffer (400*300/8 = 15000 bytes)
 */
uint8_t *epd_get_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif // EPD_DRIVER_H

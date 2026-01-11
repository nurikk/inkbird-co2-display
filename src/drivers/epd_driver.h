#ifndef EPD_DRIVER_H
#define EPD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 320
#endif

#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 480
#endif

#ifndef SCREEN_ROTATE
#define SCREEN_ROTATE 0
#endif

#define EPD_WIDTH   SCREEN_WIDTH
#define EPD_HEIGHT  SCREEN_HEIGHT

#ifndef PANEL_BGR
#define PANEL_BGR 1
#endif

#ifndef PANEL_INVERT_COLOR
#define PANEL_INVERT_COLOR 0
#endif

#ifndef PANEL_BL_ACTIVE_LOW
#define PANEL_BL_ACTIVE_LOW 0
#endif

#define EPD_PIN_MOSI    13
#define EPD_PIN_MISO    12
#define EPD_PIN_SCK     14
#define EPD_PIN_CS      15
#define EPD_PIN_DC      2
#define EPD_PIN_RST     -1
#define EPD_PIN_BL      27

/**
 * @brief Initialize the TFT display hardware
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
 * @return Pointer to framebuffer (1 byte per pixel, 0=white, 1=black)
 */
uint8_t *epd_get_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif // EPD_DRIVER_H

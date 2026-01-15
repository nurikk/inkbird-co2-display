/**
 * @file led_control.h
 * @brief RGB LED control for CO2 status indication
 *
 * Provides functions to control the RGB LED on the ESP32 board.
 * The LED changes color based on CO2 levels to provide visual status:
 * - Green: Good air quality (CO2 < 800 ppm)
 * - Yellow: Moderate (800-1400 ppm)
 * - Red: Poor air quality (CO2 >= 1400 ppm)
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize LED GPIO pins
 *
 * Configures the RGB LED pins as outputs and turns off all LEDs.
 */
void led_init(void);

/**
 * @brief Set LED color directly
 *
 * @param red Enable red LED
 * @param green Enable green LED
 * @param blue Enable blue LED
 */
void led_set_color(bool red, bool green, bool blue);

/**
 * @brief Update LED based on CO2 level from sensor 0
 *
 * Reads the current CO2 value from the first sensor and sets
 * the LED color accordingly:
 * - CO2 >= 1400 ppm: Red
 * - CO2 >= 800 ppm: Yellow (red + green)
 * - CO2 < 800 ppm: Green
 */
void led_update_from_co2(void);

#ifdef __cplusplus
}
#endif

#endif // LED_CONTROL_H

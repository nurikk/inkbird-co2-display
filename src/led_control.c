/**
 * @file led_control.c
 * @brief RGB LED control implementation
 *
 * Controls an RGB LED connected to GPIO pins for visual CO2 status indication.
 * LED is active-low (GPIO low = LED on).
 */

#include <stdbool.h>

#include "driver/gpio.h"

#include "led_control.h"
#include "sensor_data.h"

// LED GPIO pins (directly on the board)
#define LED_RED_PIN     GPIO_NUM_4
#define LED_GREEN_PIN   GPIO_NUM_17
#define LED_BLUE_PIN    GPIO_NUM_16

// LED CO2 thresholds
#define LED_CO2_RED_THRESHOLD    1400    // CO2 ppm threshold for red LED
#define LED_CO2_YELLOW_THRESHOLD 800     // CO2 ppm threshold for yellow LED

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_RED_PIN) | (1ULL << LED_GREEN_PIN) | (1ULL << LED_BLUE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Turn off all LEDs (active low, so set high)
    gpio_set_level(LED_RED_PIN, 1);
    gpio_set_level(LED_GREEN_PIN, 1);
    gpio_set_level(LED_BLUE_PIN, 1);
}

void led_set_color(bool red, bool green, bool blue)
{
    // LEDs are active-low
    gpio_set_level(LED_RED_PIN, !red);
    gpio_set_level(LED_GREEN_PIN, !green);
    gpio_set_level(LED_BLUE_PIN, !blue);
}

void led_update_from_co2(void)
{
    sensor_data_t *sensor = sensor_data_get(0);
    if (!sensor || !sensor->connected || sensor->current.co2_ppm == 0) {
        return;
    }

    uint16_t co2 = sensor->current.co2_ppm;
    if (co2 >= LED_CO2_RED_THRESHOLD) {
        led_set_color(true, false, false);    // Red
    } else if (co2 >= LED_CO2_YELLOW_THRESHOLD) {
        led_set_color(true, true, false);     // Yellow
    } else {
        led_set_color(false, true, false);    // Green
    }
}

/**
 * @file synthetic_data.c
 * @brief Synthetic sensor data generator for demo/testing
 *
 * Generates realistic CO2, temperature, and humidity data patterns:
 * - Sensor 1: Office - gradual rise during day, fall at night
 * - Sensor 2: Meeting room - spiky pattern (meetings)
 * - Sensor 3: Enclosed space - slow accumulation
 * - Sensor 4: Outdoor/reference - stable low baseline
 */

#include <stdlib.h>
#include <math.h>
#include "synthetic_data.h"
#include "sensor_data.h"

// Simple pseudo-random number generator state
static uint32_t s_rand_state = 12345;

// Time step counter
static uint32_t s_step = 0;

// Current values for each sensor (for smooth transitions)
static float s_co2[SENSOR_COUNT];
static float s_temp[SENSOR_COUNT];
static float s_humidity[SENSOR_COUNT];

// Sensor behavior parameters
typedef struct {
    float co2_base;      // Base CO2 level
    float co2_amplitude; // Variation amplitude
    float co2_noise;     // Random noise level
    float temp_base;     // Base temperature (°C)
    float humidity_base; // Base humidity (%)
} sensor_profile_t;

static const sensor_profile_t s_profiles[SENSOR_COUNT] = {
    // Sensor 1: Office room - moderate variation
    { .co2_base = 650, .co2_amplitude = 200, .co2_noise = 30,
      .temp_base = 23.0, .humidity_base = 45.0 },
    
    // Sensor 2: Meeting room - high variation, spikes
    { .co2_base = 550, .co2_amplitude = 500, .co2_noise = 50,
      .temp_base = 22.5, .humidity_base = 50.0 },
    
    // Sensor 3: Enclosed space - tends high
    { .co2_base = 900, .co2_amplitude = 400, .co2_noise = 40,
      .temp_base = 24.0, .humidity_base = 40.0 },
    
    // Sensor 4: Outdoor reference - stable, low
    { .co2_base = 415, .co2_amplitude = 25, .co2_noise = 10,
      .temp_base = 21.0, .humidity_base = 55.0 },
};

// Simple pseudo-random float [0, 1)
static float rand_float(void)
{
    s_rand_state = s_rand_state * 1103515245 + 12345;
    return (float)(s_rand_state & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

// Random float in range [-1, 1]
static float rand_signed(void)
{
    return rand_float() * 2.0f - 1.0f;
}

// Clamp value to range
static float clamp(float val, float min_val, float max_val)
{
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

void synthetic_data_init(void)
{
    // Seed with a different value each time (or use esp_random() in real code)
    s_rand_state = 42;
    s_step = 0;
    
    // Initialize current values to base levels
    for (int i = 0; i < SENSOR_COUNT; i++) {
        s_co2[i] = s_profiles[i].co2_base;
        s_temp[i] = s_profiles[i].temp_base;
        s_humidity[i] = s_profiles[i].humidity_base;
    }
}

static void update_sensor(uint8_t index)
{
    const sensor_profile_t *profile = &s_profiles[index];
    float t = (float)s_step;
    
    // Different patterns for each sensor
    float pattern = 0;
    
    switch (index) {
        case 0:
            // Sensor 1: Slow sine wave (office daily pattern)
            pattern = sinf(t * 0.05f) * 0.8f + sinf(t * 0.15f) * 0.2f;
            break;
            
        case 1:
            // Sensor 2: Spiky pattern (meetings)
            // Create occasional "meeting" spikes
            pattern = sinf(t * 0.08f) * 0.3f;
            if ((s_step % 15) < 5) {
                // Meeting in progress - CO2 rises
                pattern += 0.7f * (1.0f - cosf((float)(s_step % 15) * 0.6f));
            }
            break;
            
        case 2:
            // Sensor 3: Gradual accumulation with occasional ventilation
            pattern = sinf(t * 0.03f) * 0.5f;
            // Add slow drift upward
            pattern += 0.3f * sinf(t * 0.01f + 1.0f);
            // Occasional ventilation drop
            if ((s_step % 30) == 0) {
                s_co2[index] -= 150;
            }
            break;
            
        case 3:
            // Sensor 4: Stable outdoor - just small variations
            pattern = sinf(t * 0.02f) * 0.3f + sinf(t * 0.07f) * 0.2f;
            break;
    }
    
    // Update CO2 with pattern and noise
    float target_co2 = profile->co2_base + profile->co2_amplitude * pattern;
    target_co2 += profile->co2_noise * rand_signed();
    
    // Smooth transition (low-pass filter)
    s_co2[index] = s_co2[index] * 0.7f + target_co2 * 0.3f;
    s_co2[index] = clamp(s_co2[index], 350, 2500);
    
    // Temperature - slow variation
    float temp_variation = sinf(t * 0.02f + index) * 1.5f + rand_signed() * 0.3f;
    s_temp[index] = s_temp[index] * 0.9f + (profile->temp_base + temp_variation) * 0.1f;
    s_temp[index] = clamp(s_temp[index], 15.0f, 35.0f);
    
    // Humidity - inverse correlation with temperature, slow variation
    float humidity_variation = -temp_variation * 2.0f + rand_signed() * 2.0f;
    humidity_variation += sinf(t * 0.03f + index * 2) * 5.0f;
    s_humidity[index] = s_humidity[index] * 0.9f + 
                        (profile->humidity_base + humidity_variation) * 0.1f;
    s_humidity[index] = clamp(s_humidity[index], 20.0f, 80.0f);
    
    // Create reading and update sensor data
    sensor_reading_t reading = {
        .co2_ppm = (uint16_t)s_co2[index],
        .temperature = (int16_t)(s_temp[index] * 10.0f),  // 0.1°C units
        .humidity = (uint16_t)(s_humidity[index] * 10.0f), // 0.1% units
        .timestamp = s_step * 60000  // Pretend each step is 1 minute
    };
    
    sensor_data_update(index, &reading);
}

void synthetic_data_update(void)
{
    s_step++;
    
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        update_sensor(i);
    }
}

void synthetic_data_prefill_history(uint8_t points)
{
    if (points > SENSOR_HISTORY_SIZE) {
        points = SENSOR_HISTORY_SIZE;
    }
    
    // Reset step counter
    s_step = 0;
    
    // Initialize values
    for (int i = 0; i < SENSOR_COUNT; i++) {
        s_co2[i] = s_profiles[i].co2_base;
        s_temp[i] = s_profiles[i].temp_base;
        s_humidity[i] = s_profiles[i].humidity_base;
    }
    
    // Generate history
    for (uint8_t p = 0; p < points; p++) {
        synthetic_data_update();
    }
}

uint32_t synthetic_data_get_step(void)
{
    return s_step;
}

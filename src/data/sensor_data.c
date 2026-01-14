/**
 * @file sensor_data.c
 * @brief Sensor data structures and history management
 */

#include <string.h>
#include <stdio.h>
#include "sensor_data.h"

// Static sensor data storage
static sensor_data_t s_sensors[SENSOR_COUNT];

// Temporary buffer for returning history in order
static int16_t s_history_buffer[SENSOR_HISTORY_SIZE];

void sensor_data_init(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        memset(&s_sensors[i], 0, sizeof(sensor_data_t));
        s_sensors[i].id = i;
        snprintf(s_sensors[i].name, sizeof(s_sensors[i].name), "Sensor %d", i + 1);
        s_sensors[i].connected = false;
        s_sensors[i].history_head = 0;
        s_sensors[i].history_count = 0;
        
        // Initialize history with invalid marker
        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            s_sensors[i].co2_history[j] = -1;  // -1 = no data
        }
    }
}

sensor_data_t *sensor_data_get(uint8_t index)
{
    if (index >= SENSOR_COUNT) {
        return NULL;
    }
    return &s_sensors[index];
}

void sensor_data_update(uint8_t index, const sensor_reading_t *reading)
{
    if (index >= SENSOR_COUNT || reading == NULL) {
        return;
    }
    
    sensor_data_t *sensor = &s_sensors[index];
    
    // Update current reading
    sensor->current = *reading;
    sensor->connected = true;
    
    // Add to history ring buffer
    sensor->co2_history[sensor->history_head] = (int16_t)reading->co2_ppm;
    sensor->history_head = (sensor->history_head + 1) % SENSOR_HISTORY_SIZE;
    
    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        sensor->history_count++;
    }
}

void sensor_data_add_history(uint8_t index, uint16_t co2_ppm)
{
    if (index >= SENSOR_COUNT) {
        return;
    }
    
    sensor_data_t *sensor = &s_sensors[index];
    
    // Add to history ring buffer only (don't update current reading)
    sensor->co2_history[sensor->history_head] = (int16_t)co2_ppm;
    sensor->history_head = (sensor->history_head + 1) % SENSOR_HISTORY_SIZE;
    
    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        sensor->history_count++;
    }
}

co2_status_t sensor_data_get_co2_status(uint16_t co2_ppm)
{
    return sensor_data_get_co2_status_ex(co2_ppm, CO2_LEVEL_GOOD_DEFAULT, CO2_LEVEL_WARNING_DEFAULT);
}

co2_status_t sensor_data_get_co2_status_ex(uint16_t co2_ppm, uint16_t low_ppm, uint16_t high_ppm)
{
    uint16_t mid = (low_ppm + high_ppm) / 2;
    if (co2_ppm < low_ppm) {
        return CO2_STATUS_GOOD;
    } else if (co2_ppm < mid) {
        return CO2_STATUS_MODERATE;
    } else if (co2_ppm < high_ppm) {
        return CO2_STATUS_WARNING;
    } else {
        return CO2_STATUS_ALERT;
    }
}

const char *sensor_data_get_status_text(co2_status_t status)
{
    switch (status) {
        case CO2_STATUS_GOOD:
            return "[OK]";
        case CO2_STATUS_MODERATE:
            return "[--]";
        case CO2_STATUS_WARNING:
            return "[!]";
        case CO2_STATUS_ALERT:
            return "[!!!]";
        case CO2_STATUS_OFFLINE:
            return "[OFF]";
        default:
            return "[?]";
    }
}

const int16_t *sensor_data_get_co2_history(uint8_t index, uint8_t *out_count)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    
    sensor_data_t *sensor = &s_sensors[index];
    
    if (out_count) {
        *out_count = sensor->history_count;
    }
    
    // Reorder ring buffer into chronological order
    // Oldest data first, newest last
    if (sensor->history_count == 0) {
        return s_history_buffer;
    }
    
    uint8_t start;
    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        // Buffer not full yet, start from 0
        start = 0;
    } else {
        // Buffer full, start from head (oldest)
        start = sensor->history_head;
    }
    
    for (uint8_t i = 0; i < sensor->history_count; i++) {
        uint8_t src_idx = (start + i) % SENSOR_HISTORY_SIZE;
        s_history_buffer[i] = sensor->co2_history[src_idx];
    }
    
    // Fill remaining with last value (for charts)
    int16_t last_val = sensor->history_count > 0 ? 
                       s_history_buffer[sensor->history_count - 1] : 0;
    for (uint8_t i = sensor->history_count; i < SENSOR_HISTORY_SIZE; i++) {
        s_history_buffer[i] = last_val;
    }
    
    return s_history_buffer;
}

void sensor_data_set_name(uint8_t index, const char *name)
{
    if (index >= SENSOR_COUNT || name == NULL) {
        return;
    }

    strncpy(s_sensors[index].name, name, sizeof(s_sensors[index].name) - 1);
    s_sensors[index].name[sizeof(s_sensors[index].name) - 1] = '\0';
}

void sensor_data_set_status(uint8_t index, const char *status)
{
    if (index >= SENSOR_COUNT) {
        return;
    }

    if (status == NULL) {
        s_sensors[index].status_text[0] = '\0';
    } else {
        strncpy(s_sensors[index].status_text, status, sizeof(s_sensors[index].status_text) - 1);
        s_sensors[index].status_text[sizeof(s_sensors[index].status_text) - 1] = '\0';
    }
}

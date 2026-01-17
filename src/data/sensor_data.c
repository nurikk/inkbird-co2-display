/**
 * @file sensor_data.c
 * @brief Sensor data structures and history management
 */

#include <string.h>
#include <stdio.h>
#include "sensor_data.h"

// Static sensor data storage
static sensor_data_t s_sensors[SENSOR_COUNT];

// Temporary buffers for returning history in order
static int16_t s_co2_history_buffer[SENSOR_HISTORY_SIZE];
static int16_t s_temp_history_buffer[SENSOR_HISTORY_SIZE];
static int16_t s_hum_history_buffer[SENSOR_HISTORY_SIZE];
static int16_t s_pres_history_buffer[SENSOR_HISTORY_SIZE];
static uint16_t s_time_offsets_buffer[SENSOR_HISTORY_SIZE];

// Last interval for timestamp reconstruction
static uint8_t s_last_interval[SENSOR_COUNT];
static uint16_t s_cumulative_minutes[SENSOR_COUNT];

void sensor_data_init(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        memset(&s_sensors[i], 0, sizeof(sensor_data_t));
        s_sensors[i].id = i;
        snprintf(s_sensors[i].name, sizeof(s_sensors[i].name), "Sensor %d", i + 1);
        s_sensors[i].connected = false;
        s_sensors[i].history_head = 0;
        s_sensors[i].history_count = 0;
        s_sensors[i].total_minutes = 0;

        for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
            s_sensors[i].co2_history[j] = -1;
            s_sensors[i].temp_history[j] = -1;
            s_sensors[i].hum_history[j] = -1;
            s_sensors[i].pres_history[j] = -1;
            s_sensors[i].time_offsets[j] = 0;
        }

        s_last_interval[i] = 0;
        s_cumulative_minutes[i] = 0;
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

    sensor->current = *reading;
    sensor->connected = true;

    if (sensor->history_count > 0) {
        s_cumulative_minutes[index] += 1;
    }

    sensor->co2_history[sensor->history_head] = (int16_t)reading->co2_ppm;
    sensor->temp_history[sensor->history_head] = reading->temperature;
    sensor->hum_history[sensor->history_head] = (int16_t)reading->humidity;
    sensor->pres_history[sensor->history_head] = (int16_t)reading->pressure;
    sensor->time_offsets[sensor->history_head] = s_cumulative_minutes[index];
    sensor->history_head = (sensor->history_head + 1) % SENSOR_HISTORY_SIZE;

    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        sensor->history_count++;
    }

    sensor->total_minutes = s_cumulative_minutes[index];
}

void sensor_data_add_history(uint8_t index, uint16_t co2_ppm)
{
    sensor_data_add_history_full(index, co2_ppm, -1, 0, 0);
}

void sensor_data_add_history_full(uint8_t index, uint16_t co2_ppm,
                                   int16_t temperature, uint16_t humidity, uint16_t pressure)
{
    if (index >= SENSOR_COUNT) {
        return;
    }

    sensor_data_t *sensor = &s_sensors[index];

    sensor->co2_history[sensor->history_head] = (int16_t)co2_ppm;
    sensor->temp_history[sensor->history_head] = temperature;
    sensor->hum_history[sensor->history_head] = (int16_t)humidity;
    sensor->pres_history[sensor->history_head] = (int16_t)pressure;
    sensor->time_offsets[sensor->history_head] = 0;
    sensor->history_head = (sensor->history_head + 1) % SENSOR_HISTORY_SIZE;

    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        sensor->history_count++;
    }
}

void sensor_data_clear_history(uint8_t index)
{
    if (index >= SENSOR_COUNT) {
        return;
    }

    sensor_data_t *sensor = &s_sensors[index];
    sensor->history_head = 0;
    sensor->history_count = 0;
    sensor->total_minutes = 0;

    for (int j = 0; j < SENSOR_HISTORY_SIZE; j++) {
        sensor->co2_history[j] = -1;
        sensor->temp_history[j] = -1;
        sensor->hum_history[j] = -1;
        sensor->pres_history[j] = -1;
        sensor->time_offsets[j] = 0;
    }

    s_last_interval[index] = 0;
    s_cumulative_minutes[index] = 0;
}

void sensor_data_add_history_with_interval(uint8_t index, uint16_t co2_ppm,
                                            int16_t temperature, uint16_t humidity,
                                            uint16_t pressure, uint8_t interval_mins)
{
    if (index >= SENSOR_COUNT) {
        return;
    }

    sensor_data_t *sensor = &s_sensors[index];

    if (sensor->history_count == 0) {
        s_cumulative_minutes[index] = 0;
        s_last_interval[index] = interval_mins;
    } else {
        uint8_t prev = s_last_interval[index];
        uint8_t curr = interval_mins;
        uint16_t elapsed;

        // Records are in OLDEST-to-NEWEST order after history download reordering
        // So time progresses forward: interval values generally increase
        // e.g., 17→18→19→...→59→0→1→...
        if (curr > prev) {
            // Normal forward progression: 17→18 = 1 minute
            elapsed = curr - prev;
        } else if (curr < prev) {
            // Hour rollover: 59→0 = 1 minute
            elapsed = (60 - prev) + curr;
        } else {
            // Same minute (duplicate record or same-minute reading)
            elapsed = 0;
        }

        s_cumulative_minutes[index] += elapsed;
        s_last_interval[index] = curr;
    }

    sensor->co2_history[sensor->history_head] = (int16_t)co2_ppm;
    sensor->temp_history[sensor->history_head] = temperature;
    sensor->hum_history[sensor->history_head] = (int16_t)humidity;
    sensor->pres_history[sensor->history_head] = (int16_t)pressure;
    sensor->time_offsets[sensor->history_head] = s_cumulative_minutes[index];
    sensor->history_head = (sensor->history_head + 1) % SENSOR_HISTORY_SIZE;

    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        sensor->history_count++;
    }

    sensor->total_minutes = s_cumulative_minutes[index];
}

uint16_t sensor_data_get_total_minutes(uint8_t index)
{
    if (index >= SENSOR_COUNT) {
        return 0;
    }
    return s_sensors[index].total_minutes;
}

const uint16_t *sensor_data_get_time_offsets(uint8_t index, uint8_t *out_count)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    sensor_data_t *sensor = &s_sensors[index];

    if (out_count) {
        *out_count = sensor->history_count;
    }

    if (sensor->history_count == 0) {
        return s_time_offsets_buffer;
    }

    uint8_t start;
    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        start = 0;
    } else {
        start = sensor->history_head;
    }

    uint16_t total = sensor->total_minutes;
    for (uint8_t i = 0; i < sensor->history_count; i++) {
        uint8_t src_idx = (start + i) % SENSOR_HISTORY_SIZE;
        s_time_offsets_buffer[i] = total - sensor->time_offsets[src_idx];
    }

    for (uint8_t i = sensor->history_count; i < SENSOR_HISTORY_SIZE; i++) {
        s_time_offsets_buffer[i] = 0;
    }

    return s_time_offsets_buffer;
}

static int16_t s_filtered_co2_buffer[SENSOR_HISTORY_SIZE];

const int16_t *sensor_data_get_co2_history_filtered(uint8_t index, uint16_t max_minutes, uint8_t *out_count)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    sensor_data_t *sensor = &s_sensors[index];

    if (sensor->history_count == 0) {
        if (out_count) *out_count = 0;
        return s_filtered_co2_buffer;
    }

    uint8_t start;
    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        start = 0;
    } else {
        start = sensor->history_head;
    }

    uint16_t total = sensor->total_minutes;
    uint8_t filtered_count = 0;
    uint8_t first_valid_idx = 0;
    bool found_start = false;

    for (uint8_t i = 0; i < sensor->history_count; i++) {
        uint8_t src_idx = (start + i) % SENSOR_HISTORY_SIZE;
        uint16_t minutes_ago = total - sensor->time_offsets[src_idx];

        if (minutes_ago <= max_minutes) {
            if (!found_start) {
                first_valid_idx = i;
                found_start = true;
            }
            filtered_count++;
        }
    }

    if (filtered_count == 0) {
        if (out_count) *out_count = 0;
        return s_filtered_co2_buffer;
    }

    for (uint8_t i = 0; i < filtered_count; i++) {
        uint8_t src_idx = (start + first_valid_idx + i) % SENSOR_HISTORY_SIZE;
        s_filtered_co2_buffer[i] = sensor->co2_history[src_idx];
    }

    for (uint8_t i = filtered_count; i < SENSOR_HISTORY_SIZE; i++) {
        s_filtered_co2_buffer[i] = s_filtered_co2_buffer[filtered_count - 1];
    }

    if (out_count) *out_count = filtered_count;
    return s_filtered_co2_buffer;
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
        return s_co2_history_buffer;
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
        s_co2_history_buffer[i] = sensor->co2_history[src_idx];
    }
    
    // Fill remaining with last value (for charts)
    int16_t last_val = sensor->history_count > 0 ? 
                       s_co2_history_buffer[sensor->history_count - 1] : 0;
    for (uint8_t i = sensor->history_count; i < SENSOR_HISTORY_SIZE; i++) {
        s_co2_history_buffer[i] = last_val;
    }
    
    return s_co2_history_buffer;
}

static const int16_t *get_history_generic(uint8_t index, uint8_t *out_count,
                                           int16_t *src_history, int16_t *dst_buffer)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    sensor_data_t *sensor = &s_sensors[index];

    if (out_count) {
        *out_count = sensor->history_count;
    }

    if (sensor->history_count == 0) {
        return dst_buffer;
    }

    uint8_t start;
    if (sensor->history_count < SENSOR_HISTORY_SIZE) {
        start = 0;
    } else {
        start = sensor->history_head;
    }

    for (uint8_t i = 0; i < sensor->history_count; i++) {
        uint8_t src_idx = (start + i) % SENSOR_HISTORY_SIZE;
        dst_buffer[i] = src_history[src_idx];
    }

    int16_t last_val = sensor->history_count > 0 ?
                       dst_buffer[sensor->history_count - 1] : 0;
    for (uint8_t i = sensor->history_count; i < SENSOR_HISTORY_SIZE; i++) {
        dst_buffer[i] = last_val;
    }

    return dst_buffer;
}

const int16_t *sensor_data_get_temp_history(uint8_t index, uint8_t *out_count)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    return get_history_generic(index, out_count,
                               s_sensors[index].temp_history, s_temp_history_buffer);
}

const int16_t *sensor_data_get_hum_history(uint8_t index, uint8_t *out_count)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    return get_history_generic(index, out_count,
                               s_sensors[index].hum_history, s_hum_history_buffer);
}

const int16_t *sensor_data_get_pres_history(uint8_t index, uint8_t *out_count)
{
    if (index >= SENSOR_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    return get_history_generic(index, out_count,
                               s_sensors[index].pres_history, s_pres_history_buffer);
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

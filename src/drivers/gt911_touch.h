#ifndef GT911_TOUCH_H
#define GT911_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GT911_I2C_ADDR      0x5D

// Pin definitions - configured via platformio.ini build_flags
// Defaults for ESP32 (original board) if not defined
#ifndef GT911_PIN_SDA
#define GT911_PIN_SDA       33
#endif
#ifndef GT911_PIN_SCL
#define GT911_PIN_SCL       32
#endif
#ifndef GT911_PIN_INT
#define GT911_PIN_INT       36
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} gt911_touch_data_t;

esp_err_t gt911_init(void);
bool gt911_read(gt911_touch_data_t *data);

#ifdef __cplusplus
}
#endif

#endif

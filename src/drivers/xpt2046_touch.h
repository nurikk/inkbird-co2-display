#ifndef XPT2046_TOUCH_H
#define XPT2046_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pin definitions - configured via platformio.ini build_flags
// Defaults for ESP32 (original board) if not defined
#ifndef XPT2046_PIN_CLK
#define XPT2046_PIN_CLK     14
#endif
#ifndef XPT2046_PIN_CS
#define XPT2046_PIN_CS      33
#endif
#ifndef XPT2046_PIN_DIN
#define XPT2046_PIN_DIN     13
#endif
#ifndef XPT2046_PIN_DOUT
#define XPT2046_PIN_DOUT    12
#endif
#ifndef XPT2046_PIN_IRQ
#define XPT2046_PIN_IRQ     36
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} xpt2046_touch_data_t;

esp_err_t xpt2046_init(void);
bool xpt2046_read(xpt2046_touch_data_t *data);

#ifdef __cplusplus
}
#endif

#endif

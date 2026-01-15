#ifndef XPT2046_TOUCH_H
#define XPT2046_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XPT2046_PIN_CLK     14
#define XPT2046_PIN_CS      33
#define XPT2046_PIN_DIN     13
#define XPT2046_PIN_DOUT    12
#define XPT2046_PIN_IRQ     36

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

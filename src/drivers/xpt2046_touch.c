#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "xpt2046_touch.h"

static const char *TAG = "xpt2046";

// XPT2046 commands - note: some boards have X/Y swapped
// Standard: X=0xD0, Y=0x90; but some use X=0x90, Y=0xD0
#define XPT2046_CMD_X       0x90
#define XPT2046_CMD_Y       0xD0
#define XPT2046_CMD_Z1      0xB0
#define XPT2046_CMD_Z2      0xC0

// Screen dimensions for 2.8" TFT in landscape mode
#define XPT_SCREEN_WIDTH    320
#define XPT_SCREEN_HEIGHT   240

// Calibration values - adjust if touch accuracy is off
#define CAL_X_MIN           200
#define CAL_X_MAX           3900
#define CAL_Y_MIN           200
#define CAL_Y_MAX           3800

// Pressure thresholds for touch detection
// When touched: Z1 increases significantly (>100), Z2 decreases (<3500)
// When not touched: Z1 is low (0-50), Z2 is high (~4095)
#define MIN_PRESSURE        100
#define MAX_PRESSURE        3500

// Try SPI2_HOST (shared with display) - many boards share the SPI bus
// The display driver should have initialized the bus already
#define TOUCH_SPI_HOST      SPI2_HOST
#define TOUCH_SPI_SHARED    1  // Don't reinitialize the bus
#define XPT_SPI_CLOCK_HZ    2500000
#define XPT_SPI_BITS        24
#define XPT_ADC_SHIFT       3
#define XPT_ADC_MASK        0x0FFF
#define TOUCH_SAMPLES       4

static bool s_initialized = false;
static spi_device_handle_t s_touch_spi = NULL;

static uint16_t xpt2046_read_adc(uint8_t cmd)
{
    uint8_t tx_data[3] = { cmd, 0x00, 0x00 };
    uint8_t rx_data[3] = { 0 };

    spi_transaction_t trans = {
        .length = XPT_SPI_BITS,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    esp_err_t ret = spi_device_transmit(s_touch_spi, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(ret));
        return 0;
    }

    uint16_t value = ((rx_data[1] << 8) | rx_data[2]) >> XPT_ADC_SHIFT;
    return value & XPT_ADC_MASK;
}

esp_err_t xpt2046_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Configure IRQ pin if available
    if (XPT2046_PIN_IRQ >= 0) {
        gpio_config_t irq_conf = {
            .pin_bit_mask = (1ULL << XPT2046_PIN_IRQ),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&irq_conf);
    }

#if TOUCH_SPI_SHARED
    // SPI bus is shared with display - already initialized
    ESP_LOGI(TAG, "Using shared SPI bus (SPI2_HOST) with display");
#else
    // Initialize SPI bus for touch controller
    // Touch uses different pins than display, so needs its own bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = XPT2046_PIN_DIN,   // DIN = MOSI
        .miso_io_num = XPT2046_PIN_DOUT,  // DOUT = MISO
        .sclk_io_num = XPT2046_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    esp_err_t ret = spi_bus_initialize(TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize touch SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Touch SPI bus initialized (CLK=%d, DIN=%d, DOUT=%d)",
             XPT2046_PIN_CLK, XPT2046_PIN_DIN, XPT2046_PIN_DOUT);
#endif

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = XPT_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = XPT2046_PIN_CS,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL,
    };

    esp_err_t ret = spi_bus_add_device(TOUCH_SPI_HOST, &devcfg, &s_touch_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add touch device to SPI bus: %s", esp_err_to_name(ret));
#if !TOUCH_SPI_SHARED
        spi_bus_free(TOUCH_SPI_HOST);
#endif
        return ret;
    }
    ESP_LOGI(TAG, "Touch device added to %s (CS=%d)",
             TOUCH_SPI_SHARED ? "shared SPI2" : "SPI3", XPT2046_PIN_CS);

    uint16_t test_x = xpt2046_read_adc(XPT2046_CMD_X);
    uint16_t test_y = xpt2046_read_adc(XPT2046_CMD_Y);
    ESP_LOGI(TAG, "XPT2046 test read: X=%u, Y=%u", test_x, test_y);

    s_initialized = true;
    ESP_LOGI(TAG, "XPT2046 touch controller initialized");
    return ESP_OK;
}

// Debug counter to limit logging frequency
static uint32_t s_debug_counter = 0;

bool xpt2046_read(xpt2046_touch_data_t *data)
{
    if (!s_initialized || data == NULL) {
        return false;
    }

    data->pressed = false;
    data->x = 0;
    data->y = 0;

    uint16_t z1 = xpt2046_read_adc(XPT2046_CMD_Z1);
    uint16_t z2 = xpt2046_read_adc(XPT2046_CMD_Z2);

    // Log raw Z values periodically for debugging
    s_debug_counter++;
    if (s_debug_counter % 100 == 0 && (z1 > 10 || z2 < 4090)) {
        ESP_LOGI(TAG, "Touch Z: z1=%u, z2=%u (threshold: z1>%d, z2<%d)",
                 z1, z2, MIN_PRESSURE, MAX_PRESSURE);
    }

    if (z1 < MIN_PRESSURE || z2 > MAX_PRESSURE) {
        return false;
    }

    uint32_t raw_x = 0;
    uint32_t raw_y = 0;

    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        raw_x += xpt2046_read_adc(XPT2046_CMD_X);
        raw_y += xpt2046_read_adc(XPT2046_CMD_Y);
    }

    raw_x /= TOUCH_SAMPLES;
    raw_y /= TOUCH_SAMPLES;

    // Log raw touch values for calibration
    ESP_LOGI(TAG, "Touch RAW: x=%lu, y=%lu, z1=%u, z2=%u",
             (unsigned long)raw_x, (unsigned long)raw_y, z1, z2);

    if (raw_x < CAL_X_MIN || raw_x > CAL_X_MAX || raw_y < CAL_Y_MIN || raw_y > CAL_Y_MAX) {
        ESP_LOGW(TAG, "Touch out of calibration range");
        return false;
    }

    int32_t x = ((int32_t)raw_y - CAL_Y_MIN) * XPT_SCREEN_WIDTH / (CAL_Y_MAX - CAL_Y_MIN);
    int32_t y = ((int32_t)raw_x - CAL_X_MIN) * XPT_SCREEN_HEIGHT / (CAL_X_MAX - CAL_X_MIN);

    // Mirror X axis to match screen orientation
    x = XPT_SCREEN_WIDTH - 1 - x;
    y = XPT_SCREEN_HEIGHT - 1 - y;

    if (x < 0) x = 0;
    if (x >= XPT_SCREEN_WIDTH) x = XPT_SCREEN_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= XPT_SCREEN_HEIGHT) y = XPT_SCREEN_HEIGHT - 1;

    data->x = (uint16_t)x;
    data->y = (uint16_t)y;
    data->pressed = true;

    ESP_LOGI(TAG, "Touch FINAL: screen x=%d, y=%d", data->x, data->y);
    return true;
}

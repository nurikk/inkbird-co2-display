#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "epd_driver.h"

static const char *TAG = "display";

#define LCD_SPI_HOST         SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ   (8000000)
#define LCD_DRAW_BUF_LINES   80

#define ST7796_CMD_MADCTL    0x36
#define ST7796_MADCTL_MY     0x80
#define ST7796_MADCTL_MX     0x40
#define ST7796_MADCTL_MV     0x20
#define ST7796_MADCTL_BGR    0x08
#define ST7796_MADCTL_RGB    0x00

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint16_t *s_clear_line = NULL;

static void backlight_init(void)
{
    if (EPD_PIN_BL < 0) {
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EPD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(EPD_PIN_BL, PANEL_BL_ACTIVE_LOW ? 0 : 1);
}

static esp_err_t lcd_spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = EPD_PIN_MISO,
        .sclk_io_num = EPD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_WIDTH * EPD_HEIGHT * sizeof(uint16_t),
    };

    return spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes;
    uint16_t delay_ms;
    bool delay_only;
} st7796_init_cmd_t;

static esp_err_t panel_init_st7796(esp_lcd_panel_io_handle_t io)
{
    static const st7796_init_cmd_t init_cmds[] = {
        { .delay_ms = 120, .delay_only = true },
        { .cmd = 0x01, .delay_ms = 120 },
        { .cmd = 0x11, .delay_ms = 120 },
        { .cmd = 0xF0, .data = { 0xC3 }, .data_bytes = 1 },
        { .cmd = 0xF0, .data = { 0x96 }, .data_bytes = 1 },

        { .cmd = 0x36, .data = { 0x80 }, .data_bytes = 1 },
        { .cmd = 0x3A, .data = { 0x55 }, .data_bytes = 1 },
        { .cmd = 0xB4, .data = { 0x01 }, .data_bytes = 1 },
        { .cmd = 0xB6, .data = { 0x80, 0x02, 0x3B }, .data_bytes = 3 },
        { .cmd = 0xE8, .data = { 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 }, .data_bytes = 8 },
        { .cmd = 0xC1, .data = { 0x06 }, .data_bytes = 1 },
        { .cmd = 0xC2, .data = { 0xA7 }, .data_bytes = 1 },
        { .cmd = 0xC5, .data = { 0x18 }, .data_bytes = 1 },
        { .delay_ms = 120, .delay_only = true },
        { .cmd = 0xE0, .data = { 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B }, .data_bytes = 14 },
        { .cmd = 0xE1, .data = { 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B }, .data_bytes = 14 },
        { .delay_ms = 120, .delay_only = true },
        { .cmd = 0xF0, .data = { 0x3C }, .data_bytes = 1 },
        { .cmd = 0xF0, .data = { 0x69 }, .data_bytes = 1 },
        { .delay_ms = 120, .delay_only = true },
        { .cmd = 0x29 },
    };

    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        const st7796_init_cmd_t *cmd = &init_cmds[i];

        if (!cmd->delay_only) {
            esp_err_t ret = esp_lcd_panel_io_tx_param(io,
                                                     cmd->cmd,
                                                     cmd->data_bytes ? cmd->data : NULL,
                                                     cmd->data_bytes);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ST7796 init 0x%02X failed: %s", cmd->cmd, esp_err_to_name(ret));
                return ret;
            }
        }

        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }

    return ESP_OK;
}

static esp_err_t panel_set_madctl(uint8_t madctl)
{
    ESP_LOGI(TAG, "Setting MADCTL to 0x%02X", madctl);
    return esp_lcd_panel_io_tx_param(s_io_handle, ST7796_CMD_MADCTL, &madctl, 1);
}

esp_err_t epd_init(void)
{
    ESP_LOGI(TAG, "Initializing TFT display...");

    ESP_RETURN_ON_ERROR(lcd_spi_init(), TAG, "SPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EPD_PIN_DC,
        .cs_gpio_num = EPD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_io_handle),
                        TAG,
                        "Panel IO init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EPD_PIN_RST,
        .rgb_ele_order = PANEL_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle),
                        TAG,
                        "Panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "Panel ESP init failed");
    ESP_RETURN_ON_ERROR(panel_init_st7796(s_io_handle), TAG, "ST7796 init failed");
    if (PANEL_INVERT_COLOR) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, true), TAG, "Panel invert failed");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "Panel on failed");

    backlight_init();

    size_t line_buf_size = EPD_WIDTH * sizeof(uint16_t);
    s_clear_line = heap_caps_malloc(line_buf_size, MALLOC_CAP_DMA);
    if (s_clear_line == NULL) {
        s_clear_line = heap_caps_malloc(line_buf_size, MALLOC_CAP_DEFAULT);
    }
    if (s_clear_line == NULL) {
        ESP_LOGE(TAG, "Failed to allocate clear line buffer");
        return ESP_ERR_NO_MEM;
    }

    for (int x = 0; x < EPD_WIDTH; x++) {
        s_clear_line[x] = 0x0000;
    }

    ESP_LOGI(TAG, "TFT display initialized (%dx%d)", EPD_WIDTH, EPD_HEIGHT);
    return ESP_OK;
}

esp_err_t epd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    if (s_panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, color_data);
}

void epd_clear(void)
{
    if (s_panel_handle == NULL || s_clear_line == NULL) {
        return;
    }

    for (int y = 0; y < EPD_HEIGHT; y++) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, EPD_WIDTH, y + 1, s_clear_line);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Display clear failed at line %d: %s", y, esp_err_to_name(ret));
            return;
        }
    }
}

bool epd_is_busy(void)
{
    return false;
}

void epd_sleep(void)
{
    if (s_panel_handle != NULL) {
        esp_lcd_panel_disp_sleep(s_panel_handle, true);
    }
}

void epd_wake(void)
{
    if (s_panel_handle != NULL) {
        esp_lcd_panel_disp_sleep(s_panel_handle, false);
    }
}


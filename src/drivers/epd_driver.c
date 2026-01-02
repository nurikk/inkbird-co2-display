/**
 * @file epd_driver.c
 * @brief E-Paper display driver for LVGL integration
 *
 * Waveshare 4.2" B/W E-Paper V1 (400x300) driver adapted for LVGL.
 * Based on Waveshare official Arduino code (epd4in2.cpp).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "epd_driver.h"

static const char *TAG = "epd";

// SPI handle
static spi_device_handle_t s_spi = NULL;

// LVGL display handle
static lv_display_t *s_disp = NULL;

// Draw buffer for LVGL (partial rendering - 40 lines at a time)
// For 1-bit color: 400 pixels / 8 bits = 50 bytes per line
// 40 lines * 50 bytes = 2000 bytes
#define EPD_BUF_LINES   40
#define EPD_BUF_SIZE    ((EPD_WIDTH / 8) * EPD_BUF_LINES)
static uint8_t s_lvgl_buf[EPD_BUF_SIZE];

// Full framebuffer for e-paper (needed because e-paper requires full refresh)
// 400 * 300 / 8 = 15000 bytes
#define EPD_FB_SIZE     ((EPD_WIDTH / 8) * EPD_HEIGHT)
static uint8_t *s_framebuffer = NULL;

// Flag to track if display needs refresh
static bool s_needs_refresh = false;

// LUT tables from Waveshare for partial refresh (faster updates)
static const uint8_t lut_vcom0[] = {
    0x00, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0x00, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

static const uint8_t lut_ww[] = {
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_bw[] = {
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_bb[] = {
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_wb[] = {
    0x20, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0x10, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// ----------------------------------------------------------------------------
// Low-level GPIO/SPI functions
// ----------------------------------------------------------------------------

static void gpio_init_output(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void gpio_init_input(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void epd_cmd(uint8_t cmd)
{
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_CS, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_PIN_CS, 1);
}

static void epd_data(uint8_t data)
{
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &data };
    spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_PIN_CS, 1);
}

static void epd_data_bulk(const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    
    // Send in chunks to avoid SPI buffer limits
    const size_t chunk_size = 1024;
    while (len > 0) {
        size_t to_send = (len > chunk_size) ? chunk_size : len;
        spi_transaction_t t = { 
            .length = to_send * 8, 
            .tx_buffer = data 
        };
        spi_device_polling_transmit(s_spi, &t);
        data += to_send;
        len -= to_send;
    }
    
    gpio_set_level(EPD_PIN_CS, 1);
}

/**
 * @brief Wait for BUSY pin to go HIGH (idle)
 * 
 * Per Waveshare EPD_4IN2_ReadBusy(): polls GET_STATUS (0x71) command
 * while waiting. BUSY pin is LOW when busy, HIGH when idle.
 */
static void epd_wait_busy(void)
{
    ESP_LOGD(TAG, "Waiting for display...");
    epd_cmd(0x71);  // GET_STATUS - per Waveshare
    int count = 0;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {  // LOW = busy
        epd_cmd(0x71);  // Poll status while waiting
        vTaskDelay(pdMS_TO_TICKS(100));
        count++;
        if (count > 300) {  // 30 second timeout
            ESP_LOGE(TAG, "Timeout waiting for display!");
            return;
        }
    }
    ESP_LOGD(TAG, "Display ready");
}

/**
 * @brief Hardware reset sequence from Waveshare
 */
static void epd_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t spi_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;
    
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    
    return spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
}

/**
 * @brief Set LUT tables for full refresh
 * 
 * Per Waveshare EPD_4IN2_SetLut(): sends 36 bytes per table
 * Register assignments:
 *   0x20 = VCOM
 *   0x21 = WW (white to white)
 *   0x22 = BW (black to white)
 *   0x23 = WB (white to black)
 *   0x24 = BB (black to black)
 */
static void epd_set_lut(void)
{
    epd_cmd(0x20);  // LUT_FOR_VCOM
    for (int i = 0; i < 36; i++) epd_data(lut_vcom0[i]);
    
    epd_cmd(0x21);  // LUT_WHITE_TO_WHITE
    for (int i = 0; i < 36; i++) epd_data(lut_ww[i]);
    
    epd_cmd(0x22);  // LUT_BLACK_TO_WHITE
    for (int i = 0; i < 36; i++) epd_data(lut_bw[i]);
    
    epd_cmd(0x23);  // LUT_WHITE_TO_BLACK
    for (int i = 0; i < 36; i++) epd_data(lut_wb[i]);
    
    epd_cmd(0x24);  // LUT_BLACK_TO_BLACK
    for (int i = 0; i < 36; i++) epd_data(lut_bb[i]);
}

/**
 * @brief Initialize display with Waveshare sequence
 * 
 * Based on official Waveshare EPD_4in2.c EPD_4IN2_Init_Fast() function.
 * Must load LUT waveform tables for proper pixel driving.
 */
static void epd_init_display(void)
{
    epd_reset();
    
    epd_cmd(0x01);  // POWER_SETTING
    epd_data(0x03);  // VDS_EN, VDG_EN (internal DC-DC)
    epd_data(0x00);  // VCOM_HV, VGHL_LV[1:0] (VGH=20V, VGL=-20V)
    epd_data(0x2B);  // VDH = 15V
    epd_data(0x2B);  // VDL = -15V
    
    epd_cmd(0x06);  // BOOSTER_SOFT_START
    epd_data(0x17);  // Phase A
    epd_data(0x17);  // Phase B
    epd_data(0x17);  // Phase C
    
    epd_cmd(0x04);  // POWER_ON
    epd_wait_busy();
    
    epd_cmd(0x00);  // PANEL_SETTING
    epd_data(0xBF);  // KW mode, LUT from register, scan up, shift right
    
    epd_cmd(0x30);  // PLL_CONTROL
    epd_data(0x3C);  // 100Hz frame rate
    
    epd_cmd(0x61);  // RESOLUTION_SETTING
    epd_data(0x01);
    epd_data(0x90);  // 400
    epd_data(0x01);
    epd_data(0x2C);  // 300
    
    epd_cmd(0x82);  // VCM_DC_SETTING
    epd_data(0x12);  // VCOM DC level
    
    epd_cmd(0x50);  // VCOM_AND_DATA_INTERVAL_SETTING
    epd_data(0x97);  // CDI=9, DDX=1, VBD=1 (white border)
    
    // Load waveform LUT tables - CRITICAL for proper black/white driving
    epd_set_lut();
}

// ----------------------------------------------------------------------------
// LVGL flush callback
// ----------------------------------------------------------------------------

static void epd_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t area_h = lv_area_get_height(area);
    
    // LVGL I1 format includes an 8-byte palette at the start (2 colors * 4 bytes each)
    // Skip the palette to get to the actual pixel data
    static const int32_t I1_PALETTE_SIZE = 8;
    uint8_t *pixel_data = px_map + I1_PALETTE_SIZE;
    
    // Get the actual stride from LVGL's draw buffer
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    int32_t src_stride = draw_buf->header.stride;
    
    // LVGL I1: bit=1 when luminance > 127 (white), bit=0 when dark (black)
    // E-paper: 0xFF = white, 0x00 = black
    // Polarity matches - no inversion needed
    
    // Destination stride is always the full display width
    int32_t dst_stride = EPD_WIDTH / 8;  // 50 bytes for 400 pixels
    
    // For full-width areas starting at x=0, we can do a direct memcpy per row
    if (area->x1 == 0 && area->x2 == EPD_WIDTH - 1) {
        // Source and dest strides should match for full width
        for (int32_t y = 0; y < area_h; y++) {
            memcpy(&s_framebuffer[(area->y1 + y) * dst_stride], 
                   &pixel_data[y * src_stride], 
                   dst_stride);
        }
    } else {
        // Partial width - copy row by row
        int32_t start_byte = area->x1 / 8;
        int32_t end_byte = area->x2 / 8;
        int32_t bytes_per_row = end_byte - start_byte + 1;
        
        for (int32_t y = 0; y < area_h; y++) {
            int32_t src_offset = y * src_stride + start_byte;
            int32_t dst_offset = (area->y1 + y) * dst_stride + start_byte;
            memcpy(&s_framebuffer[dst_offset], &pixel_data[src_offset], bytes_per_row);
        }
    }
    
    s_needs_refresh = true;
    lv_display_flush_ready(disp);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

esp_err_t epd_init(void)
{
    ESP_LOGI(TAG, "Initializing 4.2\" B/W e-paper display...");
    
    // Initialize GPIOs
    gpio_init_output(EPD_PIN_RST);
    gpio_init_output(EPD_PIN_DC);
    gpio_init_output(EPD_PIN_CS);
    gpio_init_input(EPD_PIN_BUSY);
    
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    
    // Initialize SPI
    esp_err_t ret = spi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Allocate framebuffer
    s_framebuffer = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_DMA);
    if (s_framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer!");
        return ESP_ERR_NO_MEM;
    }
    memset(s_framebuffer, 0xFF, EPD_FB_SIZE);  // White
    
    // Initialize display hardware
    epd_init_display();
    
    // Perform initial clear to remove any residual image/yellowing
    ESP_LOGI(TAG, "Performing initial display clear...");
    epd_clear();
    
    ESP_LOGI(TAG, "E-paper display initialized (400x300 B/W)");
    return ESP_OK;
}

esp_err_t epd_lvgl_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL display driver...");
    
    // Create display
    s_disp = lv_display_create(EPD_WIDTH, EPD_HEIGHT);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display!");
        return ESP_FAIL;
    }
    
    // Set color format to 1-bit (I1)
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    
    // Set up draw buffer - single buffer, partial rendering
    lv_display_set_buffers(s_disp, s_lvgl_buf, NULL, sizeof(s_lvgl_buf), 
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    // Set flush callback
    lv_display_set_flush_cb(s_disp, epd_flush_cb);
    
    ESP_LOGI(TAG, "LVGL display driver initialized");
    return ESP_OK;
}

void epd_refresh(void)
{
    if (!s_needs_refresh) {
        return;
    }
    
    ESP_LOGI(TAG, "Refreshing display...");
    
    // Per Waveshare EPD_4IN2_Display():
    // Send old data (DATA_START_TRANSMISSION_1) - all 0x00
    // This provides "previous" frame state for LUT transitions
    epd_cmd(0x10);
    for (size_t i = 0; i < EPD_FB_SIZE; i++) {
        epd_data(0x00);
    }
    
    // Send new data (DATA_START_TRANSMISSION_2) - actual image
    epd_cmd(0x13);
    epd_data_bulk(s_framebuffer, EPD_FB_SIZE);
    
    // Trigger refresh - LUT already loaded during init
    epd_cmd(0x12);  // DISPLAY_REFRESH
    vTaskDelay(pdMS_TO_TICKS(10));
    epd_wait_busy();
    
    s_needs_refresh = false;
    ESP_LOGI(TAG, "Display refresh complete");
}

void epd_clear(void)
{
    ESP_LOGI(TAG, "Clearing display...");
    
    // Per Waveshare EPD_4IN2_Clear(): send 0xFF for BOTH old and new data
    // This clears to white without forcing transitions through LUT
    epd_cmd(0x10);  // DATA_START_TRANSMISSION_1 (old data)
    for (size_t i = 0; i < EPD_FB_SIZE; i++) {
        epd_data(0xFF);
    }
    
    epd_cmd(0x13);  // DATA_START_TRANSMISSION_2 (new data)
    for (size_t i = 0; i < EPD_FB_SIZE; i++) {
        epd_data(0xFF);
    }
    
    // Refresh display
    epd_cmd(0x12);  // DISPLAY_REFRESH
    vTaskDelay(pdMS_TO_TICKS(1));
    epd_wait_busy();
    
    // Update local framebuffer
    memset(s_framebuffer, 0xFF, EPD_FB_SIZE);
    s_needs_refresh = false;
}

bool epd_is_busy(void)
{
    return gpio_get_level(EPD_PIN_BUSY) == 0;  // LOW = busy
}

void epd_sleep(void)
{
    epd_cmd(0x50);  // VCOM_AND_DATA_INTERVAL_SETTING
    epd_data(0x17);  // Border floating
    
    epd_cmd(0x82);  // VCM_DC_SETTING - VCOM to 0V
    
    epd_cmd(0x00);  // PANEL_SETTING
    vTaskDelay(pdMS_TO_TICKS(100));
    
    epd_cmd(0x01);  // POWER_SETTING - VG&VS to 0V fast
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    epd_cmd(0x02);  // POWER_OFF
    epd_wait_busy();
    
    epd_cmd(0x07);  // DEEP_SLEEP
    epd_data(0xA5);
}

void epd_wake(void)
{
    epd_init_display();
}

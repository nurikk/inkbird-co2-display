/**
 * @file epd_driver.c
 * @brief E-Paper display driver (no LVGL)
 *
 * Waveshare 4.2" B/W E-Paper V1 (400x300) driver.
 * Based on Waveshare official Arduino code (epd4in2.cpp).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "epd_driver.h"

static const char *TAG = "epd";

// SPI handle
static spi_device_handle_t s_spi = NULL;

// Full framebuffer for e-paper
// 400 * 300 / 8 = 15000 bytes
#define EPD_FB_SIZE     ((EPD_WIDTH / 8) * EPD_HEIGHT)
static uint8_t *s_framebuffer = NULL;

// Full refresh LUT tables from Waveshare EPD_4in2.c
static const uint8_t lut_vcom0[] = {
    0x00, 0x17, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x17, 0x17, 0x00, 0x00, 0x02,
    0x00, 0x0A, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x0E, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

static const uint8_t lut_ww[] = {
    0x40, 0x17, 0x00, 0x00, 0x00, 0x02,
    0x90, 0x17, 0x17, 0x00, 0x00, 0x02,
    0x40, 0x0A, 0x01, 0x00, 0x00, 0x01,
    0xA0, 0x0E, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_bw[] = {
    0x40, 0x17, 0x00, 0x00, 0x00, 0x02,
    0x90, 0x17, 0x17, 0x00, 0x00, 0x02,
    0x40, 0x0A, 0x01, 0x00, 0x00, 0x01,
    0xA0, 0x0E, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_bb[] = {
    0x80, 0x17, 0x00, 0x00, 0x00, 0x02,
    0x90, 0x17, 0x17, 0x00, 0x00, 0x02,
    0x80, 0x0A, 0x01, 0x00, 0x00, 0x01,
    0x50, 0x0E, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_wb[] = {
    0x80, 0x17, 0x00, 0x00, 0x00, 0x02,
    0x90, 0x17, 0x17, 0x00, 0x00, 0x02,
    0x80, 0x0A, 0x01, 0x00, 0x00, 0x01,
    0x50, 0x0E, 0x0E, 0x00, 0x00, 0x02,
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

static void epd_wait_busy(void)
{
    ESP_LOGD(TAG, "Waiting for display...");
    epd_cmd(0x71);
    int count = 0;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        epd_cmd(0x71);
        vTaskDelay(pdMS_TO_TICKS(100));
        count++;
        if (count > 300) {
            ESP_LOGE(TAG, "Timeout waiting for display!");
            return;
        }
    }
    ESP_LOGD(TAG, "Display ready");
}

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

static void epd_set_lut(void)
{
    epd_cmd(0x20);
    for (int i = 0; i < 36; i++) epd_data(lut_vcom0[i]);
    
    epd_cmd(0x21);
    for (int i = 0; i < 36; i++) epd_data(lut_ww[i]);
    
    epd_cmd(0x22);
    for (int i = 0; i < 36; i++) epd_data(lut_bw[i]);
    
    epd_cmd(0x23);
    for (int i = 0; i < 36; i++) epd_data(lut_wb[i]);
    
    epd_cmd(0x24);
    for (int i = 0; i < 36; i++) epd_data(lut_bb[i]);
}

static void epd_init_display(void)
{
    epd_reset();
    
    epd_cmd(0x01);  // POWER_SETTING
    epd_data(0x03);
    epd_data(0x00);
    epd_data(0x2B);
    epd_data(0x2B);
    
    epd_cmd(0x06);  // BOOSTER_SOFT_START
    epd_data(0x17);
    epd_data(0x17);
    epd_data(0x17);
    
    epd_cmd(0x04);  // POWER_ON
    epd_wait_busy();
    
    epd_cmd(0x00);  // PANEL_SETTING
    epd_data(0xBF);
    
    epd_cmd(0x30);  // PLL_CONTROL
    epd_data(0x3C);
    
    epd_cmd(0x61);  // RESOLUTION_SETTING
    epd_data(0x01);
    epd_data(0x90);  // 400
    epd_data(0x01);
    epd_data(0x2C);  // 300
    
    epd_cmd(0x82);  // VCM_DC_SETTING
    epd_data(0x28);
    
    epd_cmd(0x50);  // VCOM_AND_DATA_INTERVAL_SETTING
    epd_data(0x97);
    
    epd_set_lut();
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

esp_err_t epd_init(void)
{
    ESP_LOGI(TAG, "Initializing 4.2\" B/W e-paper display...");
    
    gpio_init_output(EPD_PIN_RST);
    gpio_init_output(EPD_PIN_DC);
    gpio_init_output(EPD_PIN_CS);
    gpio_init_input(EPD_PIN_BUSY);
    
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    
    esp_err_t ret = spi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_framebuffer = heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_DMA);
    if (s_framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer!");
        return ESP_ERR_NO_MEM;
    }
    memset(s_framebuffer, 0xFF, EPD_FB_SIZE);  // White
    
    epd_init_display();
    
    ESP_LOGI(TAG, "Performing initial display clear...");
    epd_clear();
    
    ESP_LOGI(TAG, "E-paper display initialized (400x300 B/W)");
    return ESP_OK;
}

void epd_refresh(void)
{
    ESP_LOGI(TAG, "Refreshing display...");
    
    epd_cmd(0x10);
    epd_data_bulk(s_framebuffer, EPD_FB_SIZE);
    
    epd_cmd(0x13);
    epd_data_bulk(s_framebuffer, EPD_FB_SIZE);
    
    epd_cmd(0x12);
    vTaskDelay(pdMS_TO_TICKS(10));
    epd_wait_busy();
    
    ESP_LOGI(TAG, "Display refresh complete");
}

void epd_clear(void)
{
    ESP_LOGI(TAG, "Clearing display...");
    
    epd_cmd(0x10);
    for (size_t i = 0; i < EPD_FB_SIZE; i++) {
        epd_data(0xFF);
    }
    
    epd_cmd(0x13);
    for (size_t i = 0; i < EPD_FB_SIZE; i++) {
        epd_data(0xFF);
    }
    
    epd_cmd(0x12);
    vTaskDelay(pdMS_TO_TICKS(1));
    epd_wait_busy();
    
    memset(s_framebuffer, 0xFF, EPD_FB_SIZE);
}

bool epd_is_busy(void)
{
    return gpio_get_level(EPD_PIN_BUSY) == 0;
}

void epd_sleep(void)
{
    epd_cmd(0x50);
    epd_data(0x17);
    
    epd_cmd(0x82);
    
    epd_cmd(0x00);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    epd_cmd(0x01);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    epd_cmd(0x02);
    epd_wait_busy();
    
    epd_cmd(0x07);
    epd_data(0xA5);
}

void epd_wake(void)
{
    epd_init_display();
}

uint8_t *epd_get_framebuffer(void)
{
    return s_framebuffer;
}

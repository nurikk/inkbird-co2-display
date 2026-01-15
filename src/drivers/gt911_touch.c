#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "gt911_touch.h"

static const char *TAG = "gt911";

#define I2C_NUM             I2C_NUM_0
#define I2C_FREQ_HZ         400000
#define I2C_TIMEOUT_MS      100

#define GT911_REG_STATUS    0x814E
#define GT911_REG_TOUCH1    0x8150
#define GT911_REG_CONFIG    0x8047
#define GT911_REG_PRODUCT   0x8140

static bool s_initialized = false;

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

    esp_err_t ret = i2c_master_write_read_device(
        I2C_NUM,
        GT911_I2C_ADDR,
        reg_buf,
        sizeof(reg_buf),
        data,
        len,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS)
    );

    return ret;
}

static esp_err_t gt911_write_reg(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value };

    return i2c_master_write_to_device(
        I2C_NUM,
        GT911_I2C_ADDR,
        buf,
        sizeof(buf),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS)
    );
}

esp_err_t gt911_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GT911_PIN_SDA,
        .scl_io_num = GT911_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    gpio_config_t int_conf = {
        .pin_bit_mask = (1ULL << GT911_PIN_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_conf);

    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t product_id[4] = {0};
    ret = gt911_read_reg(GT911_REG_PRODUCT, product_id, 4);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read product ID: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "GT911 Product ID: %c%c%c%c",
             product_id[0], product_id[1], product_id[2], product_id[3]);

    s_initialized = true;
    ESP_LOGI(TAG, "GT911 touch controller initialized");
    return ESP_OK;
}

bool gt911_read(gt911_touch_data_t *data)
{
    if (!s_initialized || data == NULL) {
        return false;
    }

    data->pressed = false;
    data->x = 0;
    data->y = 0;

    uint8_t status = 0;
    esp_err_t ret = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    if (ret != ESP_OK) {
        return false;
    }

    uint8_t touch_count = status & 0x0F;
    bool buffer_ready = (status & 0x80) != 0;

    if (!buffer_ready) {
        return false;
    }

    gt911_write_reg(GT911_REG_STATUS, 0);

    if (touch_count > 0 && touch_count <= 5) {
        uint8_t touch_data[4];
        ret = gt911_read_reg(GT911_REG_TOUCH1, touch_data, 4);
        if (ret == ESP_OK) {
            data->x = touch_data[0] | (touch_data[1] << 8);
            data->y = touch_data[2] | (touch_data[3] << 8);
            data->pressed = true;
        }
    }

    return true;
}

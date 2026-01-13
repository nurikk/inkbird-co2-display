#include <stdio.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

#include "epd_driver.h"

static const char *TAG = "main";

#define LVGL_TICK_PERIOD_MS 5
#define LVGL_BUFFER_LINES 20

static lv_display_t *s_display = NULL;
static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;
static esp_timer_handle_t s_tick_timer = NULL;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void rgb565_to_bgr565_swap(uint16_t *buf, uint32_t px_count)
{
    for (uint32_t i = 0; i < px_count; i++) {
        uint16_t px = buf[i];
        uint16_t r = (px >> 11) & 0x1F;
        uint16_t g = (px >> 5) & 0x3F;
        uint16_t b = px & 0x1F;
        uint16_t bgr = (b << 11) | (g << 5) | r;
        buf[i] = (bgr >> 8) | (bgr << 8);
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    uint32_t px_count = (uint32_t)w * h;
    rgb565_to_bgr565_swap((uint16_t *)px_map, px_count);
    epd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static void create_checkerboard(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLL_ELASTIC);

    uint32_t colors[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00};
    const char *nums[4] = {"1", "2", "3", "4"};
    int box_size = 140;
    int gap = 10;
    int start_x = (320 - 2 * box_size - gap) / 2;
    int start_y = (480 - 2 * box_size - gap) / 2;

    for (int i = 0; i < 4; i++) {
        int row = i / 2;
        int col = i % 2;
        int x = start_x + col * (box_size + gap);
        int y = start_y + row * (box_size + gap);

        lv_obj_t *box = lv_obj_create(screen);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_size(box, box_size, box_size);
        lv_obj_set_style_bg_color(box, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLL_ELASTIC);

        lv_obj_t *lbl = lv_label_create(box);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_label_set_text(lbl, nums[i]);
        lv_obj_center(lbl);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Checkerboard Test");

    esp_err_t ret = epd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TFT init failed!");
        return;
    }

    lv_init();

    s_display = lv_display_create(320, 480);
    lv_display_set_default(s_display);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    size_t buf_size = 320 * LVGL_BUFFER_LINES * sizeof(lv_color_t);
    s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf1 == NULL) {
        s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    s_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf2 == NULL) {
        s_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    lv_display_set_buffers(s_display, s_buf1, s_buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick"
    };
    esp_timer_create(&tick_args, &s_tick_timer);
    esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_MS * 1000);

    create_checkerboard();

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

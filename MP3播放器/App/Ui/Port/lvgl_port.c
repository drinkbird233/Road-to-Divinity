#include "lvgl_port.h"
#include "lvgl/lvgl.h"
#include "lcd.h"
#include "xpt2046.h"
#include "ui.h"
#include "main.h"
#include "cmsis_os2.h"

#define UI_LCD_W 320
#define UI_LCD_H 240

static uint16_t draw_buffer[UI_LCD_W * 16] __attribute__((aligned(4)));
volatile uint32_t ui_display_errors;

static uint8_t brightness = 100;
static uint32_t sleep_timeout = 0;
static uint32_t last_touch;

void ui_port_set_brightness(uint8_t percent)
{
    brightness = percent;
    (void)brightness;
}

void ui_port_set_sleep_timeout(uint32_t milliseconds)
{
    sleep_timeout = milliseconds;
    last_touch = HAL_GetTick();
}

static void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    uint32_t count = (uint32_t)lv_area_get_width(area) * lv_area_get_height(area);

    lcd_flush_area((uint16_t)area->x1, (uint16_t)area->y1,
                   (uint16_t)area->x2, (uint16_t)area->y2,
                   (const uint16_t *)pixels, count);

    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t *input, lv_indev_data_t *data)
{
    static uint16_t x, y;

    (void)input;

    if (xpt2046_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        last_touch = HAL_GetTick();
    } else {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void ui_task(void *argument)
{
    lv_display_t *display;
    lv_indev_t *input;

    (void)argument;

    xpt2046_init();

    lv_init();
    lv_tick_set_cb(HAL_GetTick);

    display = lv_display_create(UI_LCD_W, UI_LCD_H);
    if (display == NULL) Error_Handler();
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);

    input = lv_indev_create();
    if (input == NULL) Error_Handler();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_touch);

    ui_init();

    for (;;) {
        ui_tick();
        lv_timer_handler();
        osDelay(5);
    }
}

int lvgl_port_start(void)
{
    const osThreadAttr_t attributes = {
        .name = "lvgl_ui",
        .priority = (osPriority_t)osPriorityNormal,
        .stack_size = 8192,
    };

    return osThreadNew(ui_task, NULL, &attributes) ? 0 : -1;
}

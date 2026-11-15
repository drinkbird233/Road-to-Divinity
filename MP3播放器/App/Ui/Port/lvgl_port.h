/* SPDX-License-Identifier: MIT */
#ifndef LVGL_PORT_H
#define LVGL_PORT_H
#include <stdint.h>
int lvgl_port_start(void);
void ui_port_set_brightness(uint8_t percent);
void ui_port_set_sleep_timeout(uint32_t milliseconds);
extern volatile uint32_t ui_display_errors;
#endif

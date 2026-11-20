/**
 * @file  bsp_ui.h
 * @brief 按键 / LED / 蜂鸣器 / 电池电压 / OLED 接口。
 *
 * 按键扫描必须够快才能做急停：由 ctrl 任务每 10 ms 调 bsp_ui_scan()。
 * OLED 只能在最低优先级任务里刷，且只刷变化行。
 */
#ifndef BSP_UI_H
#define BSP_UI_H

#include <stdint.h>
#include <stdbool.h>

#define BSP_UI_KEY_NUM   3u

void     bsp_ui_init(void);

/* 每 5 ms 调一次（由 ctrl 任务）：去抖 15 ms + 边沿检测 */
void     bsp_ui_scan(void);

/* 消抖后的电平：1 = 按下 */
uint8_t  bsp_ui_key(uint8_t idx);

/* 取一次"按下"事件（读后清零）：1 = 本次有新按下 */
uint8_t  bsp_ui_key_pressed(uint8_t idx);

void     bsp_ui_led(uint8_t idx, bool on);
void     bsp_ui_buzzer(bool on);

/* 电池电压 mV。内部 8 次平均，约 30 µs */
uint16_t bsp_ui_vbat_mv(void);

/* ---------------- OLED（接你现成的 SSD1306 库） ---------------- */
/* 下面两个是弱符号占位，实现后直接覆盖即可 */
void     bsp_ui_oled_init(void);
void     bsp_ui_oled_show(uint8_t line, const char *txt);

#endif /* BSP_UI_H */

#ifndef XPT2046_H
#define XPT2046_H

#include <stdbool.h>
#include <stdint.h>
#include "main.h"

/* 探索者 V3 2.8 寸电阻屏：软件 SPI */
#define XPT2046_CLK_GPIO_Port   GPIOB
#define XPT2046_CLK_Pin         GPIO_PIN_0
#define XPT2046_DO_GPIO_Port    GPIOB
#define XPT2046_DO_Pin          GPIO_PIN_2
#define XPT2046_DIN_GPIO_Port   GPIOF
#define XPT2046_DIN_Pin         GPIO_PIN_11
#define XPT2046_CS_GPIO_Port    GPIOC
#define XPT2046_CS_Pin          GPIO_PIN_13
#define XPT2046_IRQ_GPIO_Port   GPIOB
#define XPT2046_IRQ_Pin         GPIO_PIN_1

/* 先给一组起始校准值，实测后按需调整 */
#define TOUCH_X_MIN    458
#define TOUCH_X_MAX    3455
#define TOUCH_Y_MIN    690
#define TOUCH_Y_MAX    3583
#define TOUCH_SWAP_XY  0
#define TOUCH_INVERT_X 0
#define TOUCH_INVERT_Y 1

extern volatile uint16_t touch_raw_x, touch_raw_y;

void xpt2046_init(void);
bool xpt2046_read_raw(uint16_t *x, uint16_t *y);
bool xpt2046_read(uint16_t *x, uint16_t *y);

#endif /* XPT2046_H */

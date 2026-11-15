#ifndef __LCD_H
#define __LCD_H

#include <stdint.h>

/* 初始化 FSMC 与 LCD 总线，并读取控制器 ID */
void lcd_init(void);

/* 上一步读到的控制器 ID，例如 0x9341 */
uint16_t lcd_id(void);

/* 底层寄存器/数据写入口，后续面板初始化和绘图会用到 */
void lcd_wr_regno(uint16_t reg);
void lcd_wr_data(uint16_t data);

/* 整屏填充 RGB565 */
void lcd_fill(uint16_t color);

/* 把一个矩形区域按 RGB565 写入显存，供 LVGL flush 调用 */
void lcd_flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                    const uint16_t *pixels, uint32_t count);

#endif /* __LCD_H */

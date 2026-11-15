#include "lcd.h"
#include "main.h"
#include <stdio.h>

/*
 * 探索者 V3 + 2.8 寸并口屏：
 *   CS  = PG12 (FSMC_NE4)
 *   RS  = PF12 (FSMC_A6)
 *   WR  = PD5  (FSMC_NWE)
 *   RD  = PD4  (FSMC_NOE)
 *   BL  = PB15
 *   D0~D15 = PD0/1/8/9/10/14/15 + PE7~PE15
 * LCD 映射到 NE4 地址空间 0x6C000000，RS 用 A6，16 位总线：
 *   LCD_REG (RS=0) = 0x6C00007E
 *   LCD_RAM (RS=1) = 0x6C000080
 */
#define LCD_FSMC_NEX   4
#define LCD_FSMC_AX    6

#define LCD_BASE  ((uint32_t)((0x60000000u + 0x04000000u * (LCD_FSMC_NEX - 1)) | \
                              (((1u << LCD_FSMC_AX) * 2u) - 2u)))
#define LCD       ((LCD_TypeDef *)LCD_BASE)

typedef struct {
    volatile uint16_t LCD_REG;
    volatile uint16_t LCD_RAM;
} LCD_TypeDef;

#define LCD_WIDTH   320u
#define LCD_HEIGHT  240u

static uint16_t lcd_dev_id;

static void lcd_opt_delay(uint32_t count)
{
    while (count--) {
        for (volatile uint32_t i = 0; i < 12; i++) { }
    }
}

void lcd_wr_regno(uint16_t reg)
{
    LCD->LCD_REG = reg;
}

void lcd_wr_data(uint16_t data)
{
    LCD->LCD_RAM = data;
}

static uint16_t lcd_rd_data(void)
{
    volatile uint16_t ram;

    lcd_opt_delay(2);
    ram = LCD->LCD_RAM;
    return ram;
}

static void lcd_write_reg(uint16_t reg, uint16_t data)
{
    lcd_wr_regno(reg);
    lcd_wr_data(data);
}

static void lcd_backlight_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
}

/* 按正点原子的探测顺序读 ID，兼容常见的几种控制器 */
static uint16_t lcd_read_id(void)
{
    uint16_t id;

    lcd_wr_regno(0xD3);
    (void)lcd_rd_data();
    (void)lcd_rd_data();
    id = (uint16_t)(lcd_rd_data() << 8);
    id |= lcd_rd_data();
    if (id == 0x9341) return id;

    lcd_wr_regno(0x04);
    (void)lcd_rd_data();
    (void)lcd_rd_data();
    id = (uint16_t)(lcd_rd_data() << 8);
    id |= lcd_rd_data();
    if (id == 0x8552) return 0x7789;   /* ST7789 的原始 ID 是 0x8552 */
    if (id == 0x7789) return id;

    lcd_wr_regno(0xD4);
    (void)lcd_rd_data();
    (void)lcd_rd_data();
    id = (uint16_t)(lcd_rd_data() << 8);
    id |= lcd_rd_data();
    if (id == 0x5310) return id;       /* NT35310 */

    lcd_wr_regno(0xD3);
    (void)lcd_rd_data();
    (void)lcd_rd_data();
    id = (uint16_t)(lcd_rd_data() << 8);
    id |= lcd_rd_data();
    if (id == 0x7796) return id;       /* ST7796 */

    lcd_write_reg(0xF000, 0x0055);
    lcd_write_reg(0xF001, 0x00AA);
    lcd_write_reg(0xF002, 0x0052);
    lcd_write_reg(0xF003, 0x0008);
    lcd_write_reg(0xF004, 0x0001);

    lcd_wr_regno(0xC500);
    id = (uint16_t)(lcd_rd_data() << 8);
    lcd_wr_regno(0xC501);
    id |= lcd_rd_data();
    if (id == 0x5510) return id;

    lcd_wr_regno(0xD3);
    (void)lcd_rd_data();
    (void)lcd_rd_data();
    id = (uint16_t)(lcd_rd_data() << 8);
    id |= lcd_rd_data();
    if (id == 0x9806) return id;

    lcd_wr_regno(0xA1);
    (void)lcd_rd_data();
    id = lcd_rd_data();
    id = (uint16_t)(id << 8);
    id |= lcd_rd_data();
    if (id == 0x5761) id = 0x1963;

    return id;
}

static void lcd_st7789_init(void)
{
    lcd_wr_regno(0x11);         /* sleep out */
    HAL_Delay(120);

    lcd_wr_regno(0x36);         /* MADCTL：横屏 320x240，MV+MY+BGR */
    lcd_wr_data(0x68);

    lcd_wr_regno(0x3A);         /* COLMOD: 16bit */
    lcd_wr_data(0x05);

    lcd_wr_regno(0xB2);
    lcd_wr_data(0x0C); lcd_wr_data(0x0C); lcd_wr_data(0x00);
    lcd_wr_data(0x33); lcd_wr_data(0x33);

    lcd_wr_regno(0xB7);
    lcd_wr_data(0x35);

    lcd_wr_regno(0xBB);
    lcd_wr_data(0x32);

    lcd_wr_regno(0xC0);
    lcd_wr_data(0x0C);

    lcd_wr_regno(0xC2);
    lcd_wr_data(0x01);

    lcd_wr_regno(0xC3);
    lcd_wr_data(0x10);

    lcd_wr_regno(0xC4);
    lcd_wr_data(0x20);

    lcd_wr_regno(0xC6);
    lcd_wr_data(0x0F);

    lcd_wr_regno(0xD0);
    lcd_wr_data(0xA4); lcd_wr_data(0xA1);

    lcd_wr_regno(0xE0);         /* positive gamma */
    lcd_wr_data(0xD0); lcd_wr_data(0x00); lcd_wr_data(0x02);
    lcd_wr_data(0x07); lcd_wr_data(0x0A); lcd_wr_data(0x28);
    lcd_wr_data(0x32); lcd_wr_data(0x44); lcd_wr_data(0x42);
    lcd_wr_data(0x06); lcd_wr_data(0x0E); lcd_wr_data(0x12);
    lcd_wr_data(0x14); lcd_wr_data(0x17);

    lcd_wr_regno(0xE1);         /* negative gamma */
    lcd_wr_data(0xD0); lcd_wr_data(0x00); lcd_wr_data(0x02);
    lcd_wr_data(0x07); lcd_wr_data(0x0A); lcd_wr_data(0x28);
    lcd_wr_data(0x31); lcd_wr_data(0x54); lcd_wr_data(0x47);
    lcd_wr_data(0x0E); lcd_wr_data(0x1C); lcd_wr_data(0x17);
    lcd_wr_data(0x1B); lcd_wr_data(0x1E);

    lcd_wr_regno(0x2A);         /* full window: 320x240 */
    lcd_wr_data(0x00); lcd_wr_data(0x00);
    lcd_wr_data(0x01); lcd_wr_data(0x3F);

    lcd_wr_regno(0x2B);
    lcd_wr_data(0x00); lcd_wr_data(0x00);
    lcd_wr_data(0x00); lcd_wr_data(0xEF);

    lcd_wr_regno(0x29);         /* display on */
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_wr_regno(0x2A);
    lcd_wr_data(x0 >> 8); lcd_wr_data(x0 & 0xFF);
    lcd_wr_data(x1 >> 8); lcd_wr_data(x1 & 0xFF);

    lcd_wr_regno(0x2B);
    lcd_wr_data(y0 >> 8); lcd_wr_data(y0 & 0xFF);
    lcd_wr_data(y1 >> 8); lcd_wr_data(y1 & 0xFF);

    lcd_wr_regno(0x2C);         /* memory write */
}

void lcd_fill(uint16_t color)
{
    uint32_t total = (uint32_t)LCD_WIDTH * LCD_HEIGHT;

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    while (total--) {
        LCD->LCD_RAM = color;
    }
}

void lcd_flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                    const uint16_t *pixels, uint32_t count)
{
    lcd_set_window(x1, y1, x2, y2);

    while (count--) {
        LCD->LCD_RAM = *pixels++;
    }
}

void lcd_init(void)
{
    /* FSMC 与引脚已由 CubeMX 的 MX_FSMC_Init() 配好，这里只开背光 */
    lcd_backlight_init();
    HAL_Delay(50);

    lcd_dev_id = lcd_read_id();
    printf("LCD ID:%04X\r\n", lcd_dev_id);

    if (lcd_dev_id == 0x7789) {
        lcd_st7789_init();
    } else {
        printf("LCD: panel init not implemented for this ID\r\n");
    }
}

uint16_t lcd_id(void)
{
    return lcd_dev_id;
}

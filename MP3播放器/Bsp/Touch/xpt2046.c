#include "xpt2046.h"

volatile uint16_t touch_raw_x, touch_raw_y;

static void spi_delay(void)
{
    volatile unsigned int i;

    for (i = 0; i < 10; i++) {
        __NOP();
    }
}

static uint8_t transfer(uint8_t tx)
{
    uint8_t rx = 0;
    unsigned int i;

    for (i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(XPT2046_DIN_GPIO_Port, XPT2046_DIN_Pin,
                          (tx & 0x80) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        tx <<= 1;
        spi_delay();
        HAL_GPIO_WritePin(XPT2046_CLK_GPIO_Port, XPT2046_CLK_Pin, GPIO_PIN_SET);
        spi_delay();
        rx = (uint8_t)((rx << 1) |
             (HAL_GPIO_ReadPin(XPT2046_DO_GPIO_Port, XPT2046_DO_Pin) == GPIO_PIN_SET));
        HAL_GPIO_WritePin(XPT2046_CLK_GPIO_Port, XPT2046_CLK_Pin, GPIO_PIN_RESET);
    }
    return rx;
}

static uint16_t read_adc(uint8_t command)
{
    uint16_t value;

    HAL_GPIO_WritePin(XPT2046_CS_GPIO_Port, XPT2046_CS_Pin, GPIO_PIN_RESET);
    transfer(command);
    value = (uint16_t)transfer(0) << 8;
    value |= transfer(0);
    HAL_GPIO_WritePin(XPT2046_CS_GPIO_Port, XPT2046_CS_Pin, GPIO_PIN_SET);
    return (uint16_t)((value >> 3) & 0x0FFF);
}

void xpt2046_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* SCK / MOSI / CS 输出 */
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = XPT2046_CLK_Pin;
    HAL_GPIO_Init(XPT2046_CLK_GPIO_Port, &gpio);

    gpio.Pin = XPT2046_DIN_Pin;
    HAL_GPIO_Init(XPT2046_DIN_GPIO_Port, &gpio);

    gpio.Pin = XPT2046_CS_Pin;
    HAL_GPIO_Init(XPT2046_CS_GPIO_Port, &gpio);

    /* MISO / PEN 输入上拉 */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;

    gpio.Pin = XPT2046_DO_Pin;
    HAL_GPIO_Init(XPT2046_DO_GPIO_Port, &gpio);

    gpio.Pin = XPT2046_IRQ_Pin;
    HAL_GPIO_Init(XPT2046_IRQ_GPIO_Port, &gpio);

    HAL_GPIO_WritePin(XPT2046_CS_GPIO_Port, XPT2046_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(XPT2046_CLK_GPIO_Port, XPT2046_CLK_Pin, GPIO_PIN_RESET);
    read_adc(0x90);
}

bool xpt2046_read_raw(uint16_t *x, uint16_t *y)
{
    uint16_t xs[3], ys[3], t;
    unsigned int i, j;

    if (HAL_GPIO_ReadPin(XPT2046_IRQ_GPIO_Port, XPT2046_IRQ_Pin) != GPIO_PIN_RESET) {
        return false;
    }

    for (i = 0; i < 3; i++) {
        xs[i] = read_adc(0x90);
        ys[i] = read_adc(0xD0);
    }

    for (i = 0; i < 3; i++) {
        for (j = i + 1; j < 3; j++) {
            if (xs[j] < xs[i]) { t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }

    if (HAL_GPIO_ReadPin(XPT2046_IRQ_GPIO_Port, XPT2046_IRQ_Pin) != GPIO_PIN_RESET) {
        return false;
    }

    *x = xs[1];
    *y = ys[1];
    touch_raw_x = *x;
    touch_raw_y = *y;
    return true;
}

/*
 * 二维仿射校准，由四角实测点拟合：
 *   (395,3625)->(0,0)  (3555,3585)->(319,0)
 *   (330,400)->(0,239) (3560,490)->(319,239)
 * 比单纯 X/Y 缩放更好地处理面板轻微旋转和偏移。
 */
#define CAL_X_A  0.0998395f
#define CAL_X_B -0.0009701f
#define CAL_X_C -34.2211f
#define CAL_Y_A  0.0006081f
#define CAL_Y_B -0.0756070f
#define CAL_Y_C  271.4123f

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool xpt2046_read(uint16_t *x, uint16_t *y)
{
    uint16_t rx, ry;
    float fx, fy;
    int sx, sy;

    if (!xpt2046_read_raw(&rx, &ry)) return false;

    fx = CAL_X_A * (float)rx + CAL_X_B * (float)ry + CAL_X_C;
    fy = CAL_Y_A * (float)rx + CAL_Y_B * (float)ry + CAL_Y_C;

    sx = clamp_int((int)(fx + 0.5f), 0, 319);
    sy = clamp_int((int)(fy + 0.5f), 0, 239);

    *x = (uint16_t)sx;
    *y = (uint16_t)sy;
    return true;
}

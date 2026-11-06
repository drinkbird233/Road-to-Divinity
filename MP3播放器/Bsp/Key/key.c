#include "key.h"
#include <stdio.h>
#include "main.h"

#define KEY_DEBOUNCE_MS 20

#define KEY0_BIT (1U << 0)
#define KEY1_BIT (1U << 1)
#define KEY2_BIT (1U << 2)
#define WKUP_BIT (1U << 3)

static uint8_t stable_mask;             /* 消抖后当前按住的键 */
static uint8_t candidate;               /* 正在观察的新状态 */
static uint32_t candidate_tick;

static uint8_t key_read_mask(void)
{
    uint8_t m = 0;

    if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_4) == GPIO_PIN_RESET) m |= KEY0_BIT;
    if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_3) == GPIO_PIN_RESET) m |= KEY1_BIT;
    if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_2) == GPIO_PIN_RESET) m |= KEY2_BIT;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET)   m |= WKUP_BIT;

    return m;
}

void key_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* KEY0/1/2：按下接地，上拉输入 */
    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &gpio);

    /* WK_UP：按下接 VCC，下拉输入 */
    gpio.Pin = GPIO_PIN_0;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &gpio);

    stable_mask = key_read_mask();
    candidate = stable_mask;
    candidate_tick = HAL_GetTick();
}

/* 只在“按下”那一刻返回一次事件；按住不重复，松手不产生事件 */
uint8_t Key_Scan(void)
{
    uint8_t now = key_read_mask();
    uint8_t pressed;

    if (now != candidate) {
        candidate = now;
        candidate_tick = HAL_GetTick();
        return KEY_NONE;
    }

    if (candidate == stable_mask) return KEY_NONE;
    if (HAL_GetTick() - candidate_tick < KEY_DEBOUNCE_MS) return KEY_NONE;

    pressed = candidate & (uint8_t)~stable_mask;
    stable_mask = candidate;

    if (pressed & KEY0_BIT) return KEY0_PRESS;
    if (pressed & KEY1_BIT) return KEY1_PRESS;
    if (pressed & KEY2_BIT) return KEY2_PRESS;
    if (pressed & WKUP_BIT) return WKUP_PRESS;

    return KEY_NONE;
}

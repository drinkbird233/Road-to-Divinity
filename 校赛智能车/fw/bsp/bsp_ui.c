/**
 * @file  bsp_ui.c
 *
 * 【CubeMX 引脚标签】KEY1 PC1 / KEY2 PC2 / KEY3 PC3（上拉，按下为 0）
 *                    LED1 PC4 / LED2 PC5（高亮）
 *                    BUZZER PC0（高响）
 *                    VBAT_ADC PA4 = ADC1_IN4
 */
#include "bsp_ui.h"
#include "app_config.h"
#include "main.h"

extern ADC_HandleTypeDef hadc1;

static uint8_t s_key_raw[BSP_UI_KEY_NUM];
static uint8_t s_key_stable[BSP_UI_KEY_NUM];
static uint8_t s_key_event[BSP_UI_KEY_NUM];
static uint8_t s_cnt[BSP_UI_KEY_NUM];

#define BSP_UI_DEBOUNCE  3u   /* 3 × 5 ms = 15 ms */

static GPIO_TypeDef *key_port(uint8_t i)
{
    switch (i) {
    case 0: return KEY1_GPIO_Port;
    case 1: return KEY2_GPIO_Port;
    default:return KEY3_GPIO_Port;
    }
}

static uint16_t key_pin(uint8_t i)
{
    switch (i) {
    case 0: return KEY1_Pin;
    case 1: return KEY2_Pin;
    default:return KEY3_Pin;
    }
}

void bsp_ui_init(void)
{
    uint8_t i;
    for (i = 0; i < BSP_UI_KEY_NUM; i++) {
        /* ⚠️ 关键：初值取**实际电平**，不是 0。
         * 如果上电时按键已经处于按下状态（KEY1 接错/被压住/上电顺序），
         * 用 0 做初值会让它在 3 拍后产生一次"按下事件" → 车自动进入循迹模式自己跑。
         * 取实际值后，只有"松开 → 按下"的跳变才算一次有效按键。 */
        uint8_t raw = (HAL_GPIO_ReadPin(key_port(i), key_pin(i)) == GPIO_PIN_RESET) ? 1u : 0u;
        s_key_raw[i] = raw;
        s_key_stable[i] = raw;
        s_key_event[i] = 0u;
        s_cnt[i] = 0u;
    }
    bsp_ui_led(0u, false);
    bsp_ui_led(1u, false);
    bsp_ui_buzzer(false);
}

void bsp_ui_scan(void)
{
    uint8_t i;
    for (i = 0; i < BSP_UI_KEY_NUM; i++) {
        /* 上拉输入，按下读到 0 → 逻辑 1 */
        uint8_t raw = (HAL_GPIO_ReadPin(key_port(i), key_pin(i)) == GPIO_PIN_RESET) ? 1u : 0u;

        if (raw != s_key_raw[i]) {
            s_key_raw[i] = raw;
            s_cnt[i] = 0u;
        } else if (s_cnt[i] < BSP_UI_DEBOUNCE) {
            s_cnt[i]++;
            if (s_cnt[i] >= BSP_UI_DEBOUNCE && s_key_stable[i] != raw) {
                s_key_stable[i] = raw;
                if (raw) s_key_event[i] = 1u;
            }
        }
    }
}

uint8_t bsp_ui_key(uint8_t idx)
{
    return (idx < BSP_UI_KEY_NUM) ? s_key_stable[idx] : 0u;
}

uint8_t bsp_ui_key_pressed(uint8_t idx)
{
    uint8_t e;
    if (idx >= BSP_UI_KEY_NUM) return 0u;
    e = s_key_event[idx];
    s_key_event[idx] = 0u;
    return e;
}

void bsp_ui_led(uint8_t idx, bool on)
{
    GPIO_PinState st = on ? GPIO_PIN_SET : GPIO_PIN_RESET;
    if (idx == 0u)      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, st);
    else if (idx == 1u) HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, st);
}

void bsp_ui_buzzer(bool on)
{
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint16_t bsp_ui_vbat_mv(void)
{
    uint32_t acc = 0u;
    uint8_t  i;
    float    v;

    for (i = 0; i < 8u; i++) {
        if (HAL_ADC_Start(&hadc1) != HAL_OK) return 0u;
        if (HAL_ADC_PollForConversion(&hadc1, 2u) == HAL_OK) {
            acc += HAL_ADC_GetValue(&hadc1);
        }
    }
    HAL_ADC_Stop(&hadc1);

    v = ((float)acc / 8.0f) * (3.3f / 4095.0f) * CFG_VBAT_SCALE;
    return (uint16_t)(v * 1000.0f);
}

/* ---------------- OLED 占位（接你的库时覆盖） ---------------- */
#if defined(__CC_ARM)
#define BSP_WEAK __weak
#else
#define BSP_WEAK __attribute__((weak))
#endif

BSP_WEAK void bsp_ui_oled_init(void) { }
BSP_WEAK void bsp_ui_oled_show(uint8_t line, const char *txt) { (void)line; (void)txt; }

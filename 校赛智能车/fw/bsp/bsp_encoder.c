/**
 * @file  bsp_encoder.c
 *
 * 【CubeMX】TIM3 = 右编码器（PA6/PA7），TIM4 = 左编码器（PB6/PB7）
 *   编码器模式：Encoder Mode TI1 and TI2，PSC=0，ARR=65535
 *   这样每个周期 = 4 倍频，与 CFG_ENCODER_MULT=4 对应。
 */
#include "bsp_encoder.h"
#include "app_config.h"
#include "main.h"

extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim4;

static uint16_t s_last_l;
static uint16_t s_last_r;

void bsp_encoder_init(void)
{
    HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL);
    HAL_TIM_Encoder_Start(&htim4, TIM_CHANNEL_ALL);
    s_last_l = (uint16_t)__HAL_TIM_GET_COUNTER(&htim4);
    s_last_r = (uint16_t)__HAL_TIM_GET_COUNTER(&htim3);
}

void bsp_encoder_reset(void)
{
    __HAL_TIM_SET_COUNTER(&htim4, 0u);
    __HAL_TIM_SET_COUNTER(&htim3, 0u);
    s_last_l = 0u;
    s_last_r = 0u;
}

void bsp_encoder_read(int16_t *d_left, int16_t *d_right)
{
    uint16_t now_l = (uint16_t)__HAL_TIM_GET_COUNTER(&htim4);
    uint16_t now_r = (uint16_t)__HAL_TIM_GET_COUNTER(&htim3);
    int16_t  dl, dr;

    /* 无符号相减再转有符号：16 位计数器溢出也正确 */
    dl = (int16_t)(now_l - s_last_l);
    dr = (int16_t)(now_r - s_last_r);

    /* 左右通道对调：如果左电机的编码器实际接在 PA6/PA7（TIM3）上，
     * 就把 CFG_ENCODER_SWAP_LR 置 1。不纠正的话速度环会交叉耦合，车原地打转。 */
#if CFG_ENCODER_SWAP_LR
    {
        int16_t t = dl; dl = dr; dr = t;
    }
#endif

    /* 方向对齐：车前进时两侧应同号 */
    if (CFG_ENCODER_L_INVERT) dl = (int16_t)(-dl);
    if (CFG_ENCODER_R_INVERT) dr = (int16_t)(-dr);

    if (d_left)  *d_left  = dl;
    if (d_right) *d_right = dr;

    s_last_l = now_l;
    s_last_r = now_r;
}

float bsp_encoder_delta_mm(int16_t d)
{
    return (float)d * CFG_MM_PER_COUNT;
}

/* 台架显示用的"逻辑计数"：已经把左右对调和方向翻转都算进去，
 * 所以它的增减方向与 vL/vR 完全一致，不会误导。
 * （寄存器里的原始计数不变，这里只是给人看的。） */
uint16_t bsp_encoder_raw_l(void)
{
    uint16_t v;
#if CFG_ENCODER_SWAP_LR
    v = (uint16_t)__HAL_TIM_GET_COUNTER(&htim3);
#else
    v = (uint16_t)__HAL_TIM_GET_COUNTER(&htim4);
#endif
    if (CFG_ENCODER_L_INVERT) v = (uint16_t)(-(int16_t)v);
    return v;
}

uint16_t bsp_encoder_raw_r(void)
{
    uint16_t v;
#if CFG_ENCODER_SWAP_LR
    v = (uint16_t)__HAL_TIM_GET_COUNTER(&htim4);
#else
    v = (uint16_t)__HAL_TIM_GET_COUNTER(&htim3);
#endif
    if (CFG_ENCODER_R_INVERT) v = (uint16_t)(-(int16_t)v);
    return v;
}

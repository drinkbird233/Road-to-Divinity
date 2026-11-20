/**
 * @file  bsp_encoder.h
 * @brief TIM3(右) / TIM4(左) 编码器接口。16 位计数器差值法，天然抗溢出。
 */
#ifndef BSP_ENCODER_H
#define BSP_ENCODER_H

#include <stdint.h>

void  bsp_encoder_init(void);
void  bsp_encoder_reset(void);

/* 读取自上次调用以来的计数差（已考虑方向） */
void  bsp_encoder_read(int16_t *d_left, int16_t *d_right);

/* 计数差 → 位移 mm */
float bsp_encoder_delta_mm(int16_t d);

/* 直接读 TIM 计数器的当前值（台架调试用，不改变内部 last 状态） */
uint16_t bsp_encoder_raw_l(void);
uint16_t bsp_encoder_raw_r(void);

#endif /* BSP_ENCODER_H */

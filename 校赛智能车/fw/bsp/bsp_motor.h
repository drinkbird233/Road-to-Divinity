/**
 * @file  bsp_motor.h
 * @brief TB6612 双路驱动：方向 + PWM + 斜坡 + 死区补偿 + STBY。
 *
 * 安全约定：上电默认 STBY=0、PWM=0、IN=0；任何故障调用 bsp_motor_force_stop()。
 */
#ifndef BSP_MOTOR_H
#define BSP_MOTOR_H

#include <stdint.h>
#include <stdbool.h>

void  bsp_motor_init(void);
void  bsp_motor_enable(bool en);
bool  bsp_motor_is_enabled(void);

/* duty 范围 -8399..8399，内部做斜率限制；使能关闭时直接输出 0 */
void  bsp_motor_set(float duty_l, float duty_r);

/* 断电：PWM=0, IN=0, STBY=0 */
void  bsp_motor_force_stop(void);

/* 刹车：IN1=IN2=1，绕组短接（短时用，长时间发热） */
void  bsp_motor_brake(bool on);

float bsp_motor_duty_l(void);
float bsp_motor_duty_r(void);

#endif /* BSP_MOTOR_H */

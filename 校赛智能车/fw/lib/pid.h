/**
 * @file  pid.h
 * @brief PID + 一阶低通 + 限幅。纯 C，不依赖任何 HAL —— 可在 PC 上单测。
 */
#ifndef PID_H
#define PID_H

#include <stdint.h>

typedef struct {
    float kp, ki, kd;
    float i_lim;        /* 积分项限幅（输出单位） */
    float out_min, out_max;
    float i_band;       /* 积分分离带：|err| > i_band 时暂停积分（0 = 不启用）。
                         * 用来抑制阶跃跟踪时的积分饱和（windup）超调。 */
    float d_lpf_hz;     /* 微分低通截止频率 */
    float i_acc;        /* 积分累加器 */
    float d_filt;       /* 微分滤波状态 */
    float prev_meas;
    uint8_t inited;
} pid_t;

void  pid_init(pid_t *p, float kp, float ki, float kd, float out_min, float out_max);
void  pid_set_gains(pid_t *p, float kp, float ki, float kd);
void  pid_reset(pid_t *p, float meas);
float pid_step(pid_t *p, float ref, float meas, float dt);

/* ---- 一阶低通：alpha 在 init 时算好，step 里只有一次乘加 ---- */
typedef struct {
    float alpha;
    float y;
    uint8_t inited;
} lpf1_t;

void  lpf1_init(lpf1_t *f, float fs_hz, float fc_hz);
void  lpf1_reset(lpf1_t *f, float y);
float lpf1_step(lpf1_t *f, float x);

float clampf(float v, float lo, float hi);
float rate_limit(float cur, float target, float step);

#endif /* PID_H */

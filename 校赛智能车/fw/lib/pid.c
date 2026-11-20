/**
 * @file  pid.c
 */
#include "pid.h"
#include <math.h>

void pid_init(pid_t *p, float kp, float ki, float kd, float out_min, float out_max)
{
    p->kp = kp;
    p->ki = ki;
    p->kd = kd;
    p->i_lim = (out_max - out_min) * 0.3f;
    p->out_min = out_min;
    p->out_max = out_max;
    p->i_band = 0.0f;        /* 0 = 积分分离关闭 */
    p->d_lpf_hz = 0.0f;      /* 0 = 不滤波 */
    p->i_acc = 0.0f;
    p->d_filt = 0.0f;
    p->prev_meas = 0.0f;
    p->inited = 0u;
}

void pid_set_gains(pid_t *p, float kp, float ki, float kd)
{
    p->kp = kp;
    p->ki = ki;
    p->kd = kd;
}

void pid_reset(pid_t *p, float meas)
{
    p->i_acc = 0.0f;
    p->d_filt = 0.0f;
    p->prev_meas = meas;
    p->inited = 1u;
}

/*
 * 微分作用在"测量值"上（derivative on measurement），不对设定值求导，
 * 这样目标速度阶跃时不会给出微分冲激。
 * 积分用条件积分抗饱和：输出已饱和且误差还在往饱和方向推时，停止积分。
 */
float pid_step(pid_t *p, float ref, float meas, float dt)
{
    float err, d_raw, out;

    if (dt <= 0.0f) dt = 1e-3f;
    if (!p->inited) { p->prev_meas = meas; p->inited = 1u; }

    err = ref - meas;

    d_raw = -(meas - p->prev_meas) / dt;
    p->prev_meas = meas;
    if (p->d_lpf_hz > 0.0f) {
        float a = 1.0f / (1.0f + 6.2831853f * p->d_lpf_hz * dt);
        p->d_filt += a * (d_raw - p->d_filt);
        d_raw = p->d_filt;
    }

    out = p->kp * err + p->i_acc + p->kd * d_raw;

    /* 条件积分 + 积分分离：
     * 输出饱和时不再往饱和方向积分（抗饱和），
     * 误差超过 i_band 时暂停积分（抑制阶跃跟踪时的 windup 超调）。 */
    {
        int sat_hi = (out >= p->out_max) && (err > 0.0f);
        int sat_lo = (out <= p->out_min) && (err < 0.0f);
        int in_band = (p->i_band <= 0.0f) || (fabsf(err) <= p->i_band);
        if (!sat_hi && !sat_lo && in_band && p->ki > 0.0f) {
            float trial = p->i_acc + p->ki * err * dt;
            p->i_acc = clampf(trial, -p->i_lim, p->i_lim);
            out = p->kp * err + p->i_acc + p->kd * d_raw;
        }
    }

    return clampf(out, p->out_min, p->out_max);
}

/* ---------------- 一阶低通 ---------------- */

void lpf1_init(lpf1_t *f, float fs_hz, float fc_hz)
{
    float w = 6.2831853f * fc_hz / fs_hz;
    f->alpha = w / (1.0f + w);      /* 后向欧拉近似，数值稳定 */
    f->y = 0.0f;
    f->inited = 0u;
}

void lpf1_reset(lpf1_t *f, float y)
{
    f->y = y;
    f->inited = 1u;
}

float lpf1_step(lpf1_t *f, float x)
{
    if (!f->inited) { f->y = x; f->inited = 1u; return x; }
    f->y += f->alpha * (x - f->y);
    return f->y;
}

/* ---------------- 小工具 ---------------- */

float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float rate_limit(float cur, float target, float step)
{
    float d = target - cur;
    if (d >  step) d =  step;
    if (d < -step) d = -step;
    return cur + d;
}

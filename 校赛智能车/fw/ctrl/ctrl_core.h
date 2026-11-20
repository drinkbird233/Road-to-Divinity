/**
 * @file  ctrl_core.h
 * @brief 5 ms 主控：串级转向 + 双轮速度环 + 失效保护 + 状态发布。
 */
#ifndef CTRL_CORE_H
#define CTRL_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "app_types.h"

void  ctrl_init(void);

/* 由 TIM6 通知后调用，严格 5 ms 一次 */
void  ctrl_step(void);

/* 状态快照（读者用 st_snap() 取一份完整副本） */
car_state_shadow_t *ctrl_shadow(void);

/* 在线参数 */
car_params_t *ctrl_params(void);

/* 控制入口 */
void  ctrl_start(void);
void  ctrl_stop(void);
void  ctrl_estop(void);
void  ctrl_set_mode(uint8_t mode);
uint8_t ctrl_mode(void);
void  ctrl_set_remote_speed(int16_t mm_s_l, int16_t mm_s_r);
void  ctrl_calib_imu(void);
void  ctrl_note_cmd(void);       /* 收到任何上位机命令时调用，用于失联判定 */
uint32_t ctrl_tick_ms(void);

/* ---- 台架模式（调试专用，绕过控制环直接给占空比） ---- */
void  ctrl_bench(float duty_l, float duty_r);   /* 进入台架模式 */
void  ctrl_bench_stop(void);                    /* 退出台架模式，回到正常控制 */
uint8_t ctrl_bench_active(void);

/* ---- 速度保持模式（双轮速度闭环，验证 PID） ---- */
void  ctrl_hold_speed(float mm_s_l, float mm_s_r);
void  ctrl_hold_stop(void);
uint8_t ctrl_hold_active(void);

/* 运行时改速度环增益（会同步到 PID 对象） */
void  ctrl_set_speed_gains(float kp, float ki, float ff);

/* 台架诊断：速度环 PID 的内部状态 */
float ctrl_pid_iacc_l(void);
float ctrl_pid_iacc_r(void);
float ctrl_pid_ilim_l(void);
void  ctrl_pid_dump_l(char *b, int n);

#endif /* CTRL_CORE_H */

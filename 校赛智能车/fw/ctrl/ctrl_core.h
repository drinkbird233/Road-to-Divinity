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

/* ---- 定角转弯（台架验证，也是 EL_TURN90 的调参手段） ----
 * ctrl_bench_turn(90) 以当前朝向为基准右转 90°，到位/超时自动停车。
 * 用车架空或放空地，手不离急停。 */
void  ctrl_bench_turn(float deg);
void  ctrl_bench_turn_stop(void);
uint8_t ctrl_bench_turn_active(void);

/* 运行时改速度环增益（会同步到 PID 对象） */
void  ctrl_set_speed_gains(float kp, float ki, float ff);

/* 运行时改转向增益（kp 传 0 表示只改 kd） */
void  ctrl_set_steer(float kp, float kd);

/* 运行时改角速度内环增益（传负值表示不改该项） */
void  ctrl_set_rate(float rate_kp, float yaw_kp);

/* ---- 定角转弯的在线参数（T 命令） ----
 * kp : 角度环比例增益 [°/s per °]
 * acc: 刹车曲线的等效角加速度 [°/s²]，越大越快、越大越容易过冲
 * deg: 目标转角 [°]，EL_TURN90 进弯时读它
 * v  : 转弯前进速度 [mm/s]，0 = 纯原地差速转（两轮 ±dv 对称反转）
 * brake_ms : 进弯刹车相时长 [ms]，越长越稳但冲得越远
 * inner    : 内轮差速倍率，>1 让内轮多转（默认 1.0 = 对称）
 * 六项都是"传负值不改"，但 v=0 是有效值。 */
void  ctrl_set_turn(float kp, float acc, float deg, float v, float brake_ms, float inner);
void  ctrl_get_turn(float *kp, float *acc, float *deg, float *v, float *brake_ms, float *inner);

/* 直接指定航向参考（锁死盲走用） */
void  ctrl_set_yaw_ref(float yaw_deg);

/* 定义在 ctrl_core.c，ctrl_fsm 直接读（与 g_yaw_sgn 同一个风格） */
extern float g_turn_kp;
extern float g_turn_acc;
extern float g_turn_deg;
extern float g_turn_v;
extern float g_turn_brake;
extern float g_turn_inner;

/* 补线斜率（err/mm）：状态机用它区分弯道和断口 */
float ctrl_pred_slope(void);
/* 历史窗口内的平均 |误差|：断口≈0，弯道>0.5 */
float ctrl_pred_mabs(void);

/* 视觉预告：1 = 前方没有线（盲走区）。视觉断流时为 0。 */
uint8_t ctrl_vis_gap(void);

/* 台架诊断：速度环 PID 的内部状态 */
float ctrl_pid_iacc_l(void);
float ctrl_pid_iacc_r(void);
float ctrl_pid_ilim_l(void);
void  ctrl_pid_dump_l(char *b, int n);

#endif /* CTRL_CORE_H */

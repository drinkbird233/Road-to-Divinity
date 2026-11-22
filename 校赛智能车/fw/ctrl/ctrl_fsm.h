/**
 * @file  ctrl_fsm.h
 * @brief 元素状态机：把"赛道几何"翻译成"速度 + 偏置 + 增益"。
 *
 * 设计原则：每个状态切换必须"双判据"——传感器特征 + 时间/里程。
 * 单靠 8 路判元素，在弯道出口、地面反光时极易误触发，一次误判就毁一圈。
 *
 * 本文件不含任何 HAL 依赖，阈值可在算法层仿真里先粗调。
 */
#ifndef CTRL_FSM_H
#define CTRL_FSM_H

#include <stdint.h>
#include "app_types.h"
#include "bsp_track.h"
#include "bsp_imu.h"

/* 环岛类型 */
#define LOOP_CIRCLE   0u    /* 右上圆形环岛，约 330° 出 */
#define LOOP_HEX      1u    /* 六边形环岛，必须绕满 360° */

typedef struct {
    element_id_t el;
    float v_cmd;      /* 目标速度 mm/s */
    float e_bias;     /* 误差偏置（权重单位），环岛/切弯用 */
    float kp_scale;   /* 转向增益缩放，0 = 屏蔽 8 路位置项 */
    uint8_t yaw_hold; /* 1 = 允许航向保持 */
    uint8_t yaw_hard; /* 1 = 硬保持：忽略 line_ok 门限、也不让参考跟随当前朝向。
                       * 用在"确定没有线"的元素里（断口里程窗 / 强制盲走）。
                       * 为什么需要：line_ok 只要 cnt>0 就成立，断口处残留 1~2 路
                       * 假线会让 |e| 很大，而航向保持的门限是 (!line_ok || |e|<1.2)，
                       * 于是整个航向保持被跳过 —— 车在毫无修正的情况下漂走。
                       * 实测 Z 1500 时 yaw 漂了 15°，横向 200 mm，就是这个原因。 */
    uint8_t w_mode;   /* 0 = 正常循迹 PD；1 = 角度环，用下面的 yaw_target */
    float yaw_target; /* w_mode=1 时的目标航向（°），-180..180，由 EL_TURN90 设置 */
} fsm_out_t;

void  fsm_init(void);
void  fsm_arm(void);       /* 由 IDLE 进入 ARMED，等第一个有效线 */
void  fsm_stop(void);

/* 手动指定下一个环岛是按圆还是按六边形处理（默认 0 = 圆形） */
void  fsm_set_loop_kind(uint8_t kind);

/* runtime speed targets (serial 'c' command) */
/* 最后一次有效线误差（丢线后仍保留），用于判断直角弯/断口 */
float fsm_err_lv(void);

void  fsm_set_speeds(float v_straight, float v_corner);
void  fsm_get_speeds(float *v_straight, float *v_corner);

/* 盲走参数（serial 'B' 命令）。传 <=0 表示该项不改。 */
void  fsm_set_blind(int32_t max_mm, float v);
void  fsm_get_blind(int32_t *max_mm, float *v);

/* 断口里程窗（serial 'G' 命令）。d1 <= d0 表示关闭。
 * 注意：窗口必须用"跑"的里程标定，手推的数不能用 —— 电机驱动下的打滑
 * （尤其每个直角弯的刹车/原地转）会让 dist 比真实路径偏大，实测差 6% 量级。 */
void  fsm_set_gap(int32_t d0, int32_t d1);
void  fsm_get_gap(int32_t *d0, int32_t *d1);

/* 强制盲走（serial 'Z' 命令）：立刻进 EL_BLIND、忽略线、走够 mm 就停车。
 * 用途是把"盲走能不能走直"从里程标定里拆出来单独验证 —— 不依赖窗口、
 * 不依赖丢线判定，光地上摆好车就能发。mm <= 0 表示取消。 */
void  fsm_force_blind(int32_t mm);
int32_t fsm_force_blind_left(void);

/* 航向符号：+1 = 右转时 yaw 变大；-1 = 右转时 yaw 变小（WIT 模块通常是 -1）。
 * 全工程唯一来源 —— ctrl_core 的航向保持和定角转弯都读它。
 * 台架 30 秒定案：i -> 手把车头向右转 90° -> i，yaw 变大就是 +1。
 * 串口 'Y' 可在线翻转，不用重新编译。 */
extern float g_yaw_sgn;
void  fsm_toggle_yaw_sign(void);

void  fsm_step(const track_feat_t *trk, const imu_t *imu,
               int16_t speed_avg, int32_t dist_mm, uint32_t tick_ms);

const fsm_out_t *fsm_out(void);

#endif /* CTRL_FSM_H */

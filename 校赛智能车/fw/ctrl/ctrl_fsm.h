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
} fsm_out_t;

void  fsm_init(void);
void  fsm_arm(void);       /* 由 IDLE 进入 ARMED，等第一个有效线 */
void  fsm_stop(void);

/* 手动指定下一个环岛是按圆还是按六边形处理（默认 0 = 圆形） */
void  fsm_set_loop_kind(uint8_t kind);

/* runtime speed targets (serial 'c' command) */
void  fsm_set_speeds(float v_straight, float v_corner);
void  fsm_get_speeds(float *v_straight, float *v_corner);

void  fsm_step(const track_feat_t *trk, const imu_t *imu,
               int16_t speed_avg, int32_t dist_mm, uint32_t tick_ms);

const fsm_out_t *fsm_out(void);

#endif /* CTRL_FSM_H */

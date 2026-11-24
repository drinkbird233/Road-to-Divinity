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
    uint8_t w_mode;   /* 0 = 正常循迹 PD
                       * 1 = 角度环（陀螺仪闭环），用下面的 yaw_target
                       * 2 = 开环差速（不用陀螺仪），用下面的 dv_open */
    float yaw_target; /* w_mode=1 时的目标航向（°），-180..180，由 EL_TURN90 设置 */
    float dv_open;    /* w_mode=2 时的开环差速 mm/s（已带方向符号）：
                       * 正 = 左轮快右轮慢（右转），负 = 左转。
                       * 两轮变成 ±dv_open，车原地转，转速只由地面决定。 */
    uint8_t blind_lock; /* 1 = 这次盲走"写死航向"：进盲走那一刻把航向参考
                       * 直接置成 yaw_target（= 发车航向 + CFG_BLIND_YAW_OFFSET），
                       * 而不是当前朝向。只用在"数够 N 个直角弯之后那段固定盲走区"。
                       * 好处：前面每个弯差 1° 的累积误差不会带进盲走。 */
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

/* ---- 环岛速度（串口 S）---- v<=0 表示该项不改 */
void  fsm_set_loop_speed(float v_circle, float v_hex);
void  fsm_get_loop_speed(float *v_circle, float *v_hex);

/* 盲走参数（serial 'B' 命令）。传 <=0 表示该项不改。 */
void  fsm_set_blind(int32_t max_mm, float v);
void  fsm_get_blind(int32_t *max_mm, float *v);

/* ---- 转弯计数 + 模式判据（串口 'C'）----
 * 赛道进盲走区之前是连续两个右转，所以判据是
 * "已转弯数 >= min_turns 且最近两次都是 req_dir"，
 * 命中后下一次丢线直接进盲走（写死航向）。 */
void  fsm_set_turncount(int min_turns, int req_dir, int reset);

/* ---- 盲走里程窗（串口 'M'）----
 * 断口位置固定，里程是"转弯次数"之外的第二信息源。
 * max <= min 表示不检查（向后兼容）；min/max 传 0 = 该项不改。 */
void  fsm_set_blind_win(int32_t min_mm, int32_t max_mm);
void  fsm_get_blind_win(int32_t *min_mm, int32_t *max_mm);
void  fsm_get_turncount(int *cnt, int *min_turns, int *req_dir, int *used,
                        int *h1, int *h2);
float fsm_start_yaw(void);

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

/* ---- 提前入弯 / 不停车转弯（串口 'P'）----
 * 线偏到倒数第二路（|err| ≥ err_th）并持续 hold_ms 就不等丢线、直接开转。
 * on 传 <0 = 不改；err/hold 传 <=0 = 不改。
 * 详见 docs/20_moving_turn_plan.md 和 app_config.h 的 CFG_PRE_TURN_*。 */
void  fsm_set_preturn(int on, float err_th, float hold_ms);
void  fsm_get_preturn(int *on, float *err_th, float *hold_ms);
/* 1 = 当前这次 EL_TURN90 是提前入弯进来的（日志用） */
uint8_t fsm_turn_preturn(void);

/* ---- 转弯方向锁（串口 'D'）----
 * +1 强制右转 / -1 强制左转 / 0 = 看"丢线前误差"的符号（旧行为）。
 * 为什么要它：转完虽然 err≈0，但车身还斜十几度，跑 100 mm 就会漂到线另一侧，
 * 到下一个弯丢线时 err 符号是反的 → 判反方向 → 整车掉头。固定方向的赛道不要猜。 */
void  fsm_set_dir(int dir);
int   fsm_get_dir(void);

/* ---- 调试：假装"盲走已走完"（串口 K）----
 * 只为了让车能从"断口出口"直接起步单独练环岛，不影响正常整圈逻辑。 */
void  fsm_set_postblind(int on);
int   fsm_get_postblind(void);

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

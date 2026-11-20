/**
 * @file  app_types.h
 * @brief 跨任务共享的数据结构 + 无锁 seqlock。
 *
 * 设计：单写者（ctrl 任务）多处读者（comm / ui）。
 * 读者永远拿到"某一时刻的完整快照"，绝不会看到撕裂的半新半旧数据。
 * 用编译器屏障而非 __DMB，保证本文件不依赖 CMSIS/HAL —— 这样控制层可以在 PC 上编译。
 */
#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#if defined(__GNUC__) || defined(__clang__)
#define APP_BARRIER()  __asm volatile ("" ::: "memory")
#elif defined(__CC_ARM)
/* AC5：只有包含过 cmsis_armcc.h 才有 __schedule_barrier；没有就退化为无屏障
 * （单核 M4、无 cache，任务切换本身就是屏障，实际影响可忽略） */
#  if defined(__CMSIS_ARMCC_H) || defined(__CMSIS_COMPILER_H)
#    define APP_BARRIER()  __schedule_barrier()
#  else
#    define APP_BARRIER()  ((void)0)
#  endif
#else
#define APP_BARRIER()  ((void)0)
#endif

/* ---------------- 运行模式 ---------------- */
typedef enum {
    CAR_MODE_IDLE   = 0,
    CAR_MODE_TRACK  = 1,
    CAR_MODE_VISION = 2,
    CAR_MODE_REMOTE = 3,
    CAR_MODE_TEST   = 4
} car_mode_t;

/* ---------------- 失效位 ---------------- */
enum {
    FAULT_LOW_BATT   = 1u << 0,
    FAULT_IMU_LOST   = 1u << 1,
    FAULT_VIS_LOST   = 1u << 2,
    FAULT_LINK_LOST  = 1u << 3,
    FAULT_ESTOP      = 1u << 4,
    FAULT_LINE_LOST  = 1u << 5,
    FAULT_STALL      = 1u << 6
};

/* ---------------- 元素编号（与 ctrl_fsm.h 保持一致） ---------------- */
typedef enum {
    EL_NONE = 0,
    EL_START,
    EL_STRAIGHT,
    EL_CORNER,
    EL_SCURVE,
    EL_BLIND,       /* 盲走区：断线，全靠 IMU 航向 + 里程 */
    EL_LOOP_IN,     /* 环岛 / 六边形环岛：进入段 */
    EL_LOOP_RUN,    /* 绕行段（yaw 累加判出口） */
    EL_LOOP_OUT,    /* 出口段 */
    EL_CROSS,
    EL_STOP,
    EL_FAULT
} element_id_t;

/* ---------------- 状态快照 ---------------- */
typedef struct {
    /* 传感器 */
    int16_t  speed_l;        /* mm/s */
    int16_t  speed_r;        /* mm/s */
    int16_t  yaw_ddeg;       /* 0.1°，-1800..1800 */
    int16_t  gyro_ddps;      /* 0.1°/s */
    uint8_t  track_mask;     /* bit0 = x1 */
    uint8_t  track_count;
    uint8_t  track_raw;      /* 模块读回的原始字节 */
    int8_t   track_rc;       /* 上次 I2C 读取返回码（0 = 成功） */
    float    line_err;       /* 加权误差，权重单位 */
    uint8_t  line_valid;     /* 1 = 有效线 */
    int16_t  vis_x;          /* -1000..1000 */
    int16_t  vis_y;
    uint8_t  vis_valid;
    uint16_t vbat_mv;

    /* 控制 */
    uint8_t     mode;
    uint16_t    fault;
    uint8_t     element;
    int16_t     v_ref_l;     /* mm/s */
    int16_t     v_ref_r;
    int16_t     duty_l;      /* 实际下发的占空比 */
    int16_t     duty_r;
    int16_t     w_ref_ddps;  /* 0.1°/s */
    int32_t     dist_mm;     /* 累计里程 */
    uint32_t    tick_ms;
    uint32_t    ctrl_ticks;  /* 控制环累计节拍数（用来实测循环频率） */
} car_state_t;

/* 写者持有的影子结构 */
typedef struct {
    volatile uint32_t seq;   /* 奇数 = 正在写 */
    car_state_t       s;
} car_state_shadow_t;

static inline void st_pub_begin(car_state_shadow_t *sh)
{
    sh->seq++;
    APP_BARRIER();
}

static inline void st_pub_end(car_state_shadow_t *sh)
{
    APP_BARRIER();
    sh->seq++;
}

/* 读快照：成功返回 true；读者遇到写入中则重试（调用方循环两三次即可） */
static inline bool st_snap(car_state_shadow_t *sh, car_state_t *out)
{
    uint32_t a = sh->seq;
    if (a & 1u) return false;
    APP_BARRIER();
    *out = sh->s;
    APP_BARRIER();
    return (sh->seq == a);
}

/* ---------------- 在线可调参数（蓝牙 SET_PID 改这些） ---------------- */
typedef struct {
    float speed_kp;
    float speed_ki;
    float speed_ff;
    float steer_kp;
    float steer_kd;          /* 循迹误差微分（预判弯道） */
    float v_straight;        /* 直道目标速度 mm/s */
    float v_corner;          /* 弯道目标速度 mm/s */
    float rate_kp;
    float yaw_hold_kp;
    float v_max;
    float vis_kff;
    uint32_t magic;          /* 存 Flash 时用于校验 */
} car_params_t;

#define CAR_PARAMS_MAGIC  0x43415231u  /* "CAR1" */

#endif /* APP_TYPES_H */

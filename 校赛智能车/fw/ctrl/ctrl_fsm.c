/**
 * @file  ctrl_fsm.c
 *
 * 状态迁移（箭头右侧是"双判据"里的第二个）：
 *
 *   START --------看到稳定单线--------> STRAIGHT
 *   STRAIGHT ----全黑持续 15 ms-------> CROSS ----盲走 180 mm----> STRAIGHT
 *   STRAIGHT ----大误差持续 50 ms-----> CORNER ---回中 100 ms----> STRAIGHT
 *   STRAIGHT ----150 ms 内翻转 ≥2-----> SCURVE ---稳定 200 ms----> STRAIGHT
 *   STRAIGHT ----丢线 30 ms 且丢前居中-> BLIND ----重新看到线-----> STRAIGHT
 *   STRAIGHT ----一侧连通黑-----------> LOOP_IN --走 80 mm-------> LOOP_RUN
 *   LOOP_RUN ----|Σyaw| ≥ 330°/360°----> LOOP_OUT --回中---------> STRAIGHT
 *   ANY ---------STOP / 故障-----------> STOP
 *
 * 盲走区和环岛是这张图上最特别的两个元素：
 *   · 盲走区（底部断线）：没有任何线，只能靠 IMU 航向 + 里程硬走，
 *     必须设"距离预算"，超了就停车，绝不能一直往前冲。
 *   · 六边形环岛：有边线时优先跟线，缺线时才用偏置；出口用累计 yaw 判，
 *     不能用 wrap180 的瞬时差（它的返回值永远在 ±180 内，360° 判据永远不成立）。
 */
#include "ctrl_fsm.h"
#include "app_config.h"
#include "pid.h"

/* ---------------- 阈值（都要在实车上标定） ---------------- */
#define FSM_CORNER_ERR        2.0f    /* 判定进入弯道的误差阈值 */
#define FSM_CENTER_ERR        0.8f    /* 判定"回中"的误差阈值 */
#define FSM_CORNER_HOLD_MS    50u
#define FSM_CENTER_HOLD_MS    100u
#define FSM_JUNCTION_MS       15u     /* 全黑持续多久才算路口 */
#define FSM_CROSS_BLIND_MM    180     /* 路口盲走距离 [TODO] 按实际路宽改 */
#define FSM_OSC_WINDOW_MS     150u
#define FSM_OSC_FLIPS         2u
#define FSM_SCURVE_HOLD_MS    200u
#define FSM_LOOP_ENTRY_MM     80      /* 环岛入口偏置段长度 */
#define FSM_LOOP_BIAS         2.5f    /* 入口偏置强度（权重单位） */
#define FSM_LOOP_SIDE_CNT     4u      /* 一侧 ≥4 路连黑算"侧重"，判环岛入口 */
#define FSM_RELOCK_CYCLES     2u      /* 盲走/路口出口要求连续 N 拍看到线才算重新锁线 */

typedef struct {
    float    v;             /* 绕行速度 */
    float    exit_deg;      /* 累计转角到多少度算绕完 */
    uint32_t timeout_ms;    /* 超时兜底，防止卡死在环里 */
} loop_cfg_t;

static const loop_cfg_t s_loop_tbl[2] = {
    { CFG_V_LOOP_CIRCLE, 330.0f, 4000u },   /* LOOP_CIRCLE：圆形环岛 */
    { CFG_V_LOOP_HEX,    360.0f, 6000u }    /* LOOP_HEX：六边形环岛，必须整圈 */
};

static fsm_out_t s_o;

/* 运行时可调的直道 / 弯道目标速度（串口 c 命令修改，不用重新编译） */
static float     s_v_straight = CFG_V_STRAIGHT;
static float     s_v_corner   = CFG_V_CORNER;

static uint32_t  s_t_enter;
static int32_t   s_d_enter;      /* 进入状态时的里程 */
static int32_t   s_d_blind;      /* 进入盲走时的里程 */
static int32_t   s_d_loop;       /* 进入环岛时的里程 */
static uint32_t  s_hold_since;
static uint32_t  s_t_junc;
static uint32_t  s_t_corner;
static uint32_t  s_t_lost;
static float     s_err_at_loss;  /* 丢线前的误差，用来区分"盲走区"和"急弯丢线" */
static float     s_yaw_acc;      /* 连续累加的航向变化 */
static float     s_yaw_last;
static int8_t    s_sign[8];
static uint8_t   s_sign_n;
static uint32_t  s_flip_t[4];
static uint8_t   s_flip_n;
static uint8_t   s_relock;       /* 重新锁线计数 */
static uint8_t   s_armed;
static uint8_t   s_loop_kind;    /* LOOP_CIRCLE / LOOP_HEX */

static float wrap180f(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static float absf(float v) { return (v < 0.0f) ? -v : v; }

static void enter(element_id_t el, int32_t d, float yaw, uint32_t t)
{
    s_o.el    = el;
    s_t_enter = t;
    s_d_enter = d;
    s_yaw_last = yaw;
    s_hold_since = 0u;
    s_t_junc = 0u;
    s_t_corner = 0u;
    s_t_lost = 0u;
    s_relock = 0u;
}

void fsm_init(void)
{
    s_o.el = EL_NONE;
    s_o.v_cmd = 0.0f;
    s_o.e_bias = 0.0f;
    s_o.kp_scale = 1.0f;
    s_o.yaw_hold = 0u;
    s_armed = 0u;
    s_sign_n = 0u;
    s_flip_n = 0u;
    s_hold_since = 0u;
    s_t_junc = 0u;
    s_t_corner = 0u;
    s_t_lost = 0u;
    s_err_at_loss = 0.0f;
    s_yaw_acc = 0.0f;
    s_yaw_last = 0.0f;
    s_relock = 0u;
    s_loop_kind = LOOP_CIRCLE;
    s_d_blind = 0;
    s_d_loop = 0;
}

void fsm_arm(void)
{
    s_armed = 1u;
    s_o.el = EL_START;
    s_o.v_cmd = 0.0f;
    s_o.e_bias = 0.0f;
    s_o.kp_scale = 1.0f;
    s_o.yaw_hold = 0u;
}

void fsm_stop(void)
{
    s_armed = 0u;
    s_o.el = EL_STOP;
    s_o.v_cmd = 0.0f;
    s_o.e_bias = 0.0f;
}

void fsm_set_loop_kind(uint8_t kind)
{
    s_loop_kind = (kind == LOOP_HEX) ? LOOP_HEX : LOOP_CIRCLE;
}

/* 误差符号在窗口内翻转次数 */
static uint8_t osc_count(float err, uint32_t t)
{
    int8_t sg = (err > 0.3f) ? 1 : ((err < -0.3f) ? -1 : 0);
    uint8_t i, n = 0u;

    if (sg == 0) return 0u;

    if (s_sign_n > 0u && s_sign[s_sign_n - 1u] != sg) {
        s_flip_t[s_flip_n % 4u] = t;
        s_flip_n++;
    }
    if (s_sign_n < 8u) {
        s_sign[s_sign_n++] = sg;
    } else {
        for (i = 0; i < 7u; i++) s_sign[i] = s_sign[i + 1u];
        s_sign[7] = sg;
    }

    /* 只统计真正写入过的槽位，否则上电前 150 ms 会误判成 S 弯 */
    for (i = 0; i < 4u && i < s_flip_n; i++) {
        if ((uint32_t)(t - s_flip_t[i]) < FSM_OSC_WINDOW_MS) n++;
    }
    return n;
}

static void set_speed_target(float v)
{
    /* fsm 每 10 ms 跑一次 */
    float step = CFG_V_RAMP * ((float)CFG_CTRL_PERIOD_MS * 2.0f / 1000.0f);
    s_o.v_cmd = rate_limit(s_o.v_cmd, v, step);
}

const fsm_out_t *fsm_out(void)
{
    return &s_o;
}

/* ------------------------------------------------------------------ */

void fsm_step(const track_feat_t *trk, const imu_t *imu,
              int16_t speed_avg, int32_t dist_mm, uint32_t tick_ms)
{
    float yaw = imu ? imu->yaw_deg : 0.0f;
    uint32_t dwell = tick_ms - s_t_enter;
    float err = trk ? trk->err : 0.0f;
    uint8_t line = (uint8_t)(trk && trk->valid && !trk->junction);

    (void)speed_avg;

    /* 连续累加航向：环岛要转 300° 以上，wrap180 的返回值永远到不了 */
    s_yaw_acc += wrap180f(yaw - s_yaw_last);
    s_yaw_last = yaw;

    if (!s_armed) {
        s_o.v_cmd = 0.0f;
        return;
    }

    switch (s_o.el) {
    case EL_START:
        s_o.yaw_hold = 1u;
        /* 安全：**在真正看到线之前不许动**。
         * 原来这里就给了 CFG_V_CORNER，结果一上电（或按键误触发进 TRACK）
         * 车会先冲出去 300 ms 才发现没线 —— 台架上就是"烧录完电机一直转"。 */
        set_speed_target(0.0f);
        if (line) enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
        break;

    /* ================= 直道：所有元素的分发点 ================= */
    case EL_STRAIGHT:
        s_o.e_bias   = 0.0f;
        s_o.kp_scale = 1.0f;
        s_o.yaw_hold = 1u;
        set_speed_target(s_v_straight);

        if (!trk) break;

        /* 1) 全黑：路口 / 停车线（丢线不算，因为丢线时 valid=0） */
        if (trk->junction) {
            if (s_t_junc == 0u) s_t_junc = tick_ms;
            if (tick_ms - s_t_junc >= FSM_JUNCTION_MS) {
                enter(EL_CROSS, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_t_junc = 0u;
        }

        /* 2) 丢线：盲走区 or 急弯丢线。用"丢线前的误差"区分：
         *    丢之前居中 → 是断线（盲走区）；丢之前偏很大 → 是急弯冲出视野。 */
        if (!trk->valid) {
            if (s_t_lost == 0u) {
                s_t_lost = tick_ms;
                s_err_at_loss = err;
            }
            if (tick_ms - s_t_lost >= CFG_BLIND_ARM_MS) {
                if (absf(s_err_at_loss) < FSM_CENTER_ERR) {
                    s_d_blind = dist_mm;
                    enter(EL_BLIND, dist_mm, yaw, tick_ms);
                } else {
                    enter(EL_CORNER, dist_mm, yaw, tick_ms);
                }
                break;
            }
        } else {
            s_t_lost = 0u;
        }

        /* 3) 环岛入口：一侧重 + 另一侧仍有点 */
        if (line && trk->count >= FSM_LOOP_SIDE_CNT &&
            ((trk->left_edge == 0 && trk->right_edge < 6) ||
             (trk->right_edge == 7 && trk->left_edge > 1))) {
            s_d_loop = dist_mm;
            s_yaw_acc = 0.0f;                  /* 从这里开始累计转角 */
            s_o.e_bias = (err < 0.0f) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS;
            enter(EL_LOOP_IN, dist_mm, yaw, tick_ms);
            break;
        }

        /* 4) S 弯：误差高频翻转 */
        if (line && osc_count(err, tick_ms) >= FSM_OSC_FLIPS) {
            enter(EL_SCURVE, dist_mm, yaw, tick_ms);
            break;
        }

        /* 5) 弯道：大误差持续 */
        if (line && (err > FSM_CORNER_ERR || err < -FSM_CORNER_ERR)) {
            if (s_t_corner == 0u) s_t_corner = tick_ms;
            if (tick_ms - s_t_corner >= FSM_CORNER_HOLD_MS) {
                enter(EL_CORNER, dist_mm, yaw, tick_ms);
            }
        } else {
            s_t_corner = 0u;
        }
        break;

    /* ================= 盲走区（地图底部断线） ================= */
    case EL_BLIND:
        /* 屏蔽 8 路位置项，只用 IMU 保持进入时的航向 + 走直线 */
        s_o.kp_scale = 0.0f;
        s_o.e_bias   = 0.0f;
        s_o.yaw_hold = 1u;
        set_speed_target(CFG_V_BLIND);

        if (line) {
            if (++s_relock >= FSM_RELOCK_CYCLES) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_relock = 0u;
        }

        /* 距离预算：超了就停车。这是防止"冲出赛道还一直往前开"的唯一保险 */
        if ((dist_mm - s_d_blind) > CFG_BLIND_MAX_MM) {
            enter(EL_STOP, dist_mm, yaw, tick_ms);
            s_o.v_cmd = 0.0f;
        }
        break;

    /* ================= 环岛 / 六边形环岛 ================= */
    case EL_LOOP_IN:
        s_o.e_bias = (s_o.e_bias < 0.0f) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS;
        s_o.yaw_hold = 0u;
        s_o.kp_scale = 1.0f;
        set_speed_target(s_loop_tbl[s_loop_kind].v);
        if ((dist_mm - s_d_loop) > FSM_LOOP_ENTRY_MM) {
            enter(EL_LOOP_RUN, dist_mm, yaw, tick_ms);
        }
        break;

    case EL_LOOP_RUN: {
        const loop_cfg_t *cfg = &s_loop_tbl[s_loop_kind];

        s_o.yaw_hold = 0u;
        s_o.kp_scale = 1.0f;
        /* 有边线就跟线（六边形的边是实线），没线才用偏置兜住 */
        s_o.e_bias = (line) ? 0.0f
                            : ((s_o.e_bias < 0.0f) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS);
        set_speed_target(cfg->v);

        if (absf(s_yaw_acc) >= cfg->exit_deg || dwell > cfg->timeout_ms) {
            enter(EL_LOOP_OUT, dist_mm, yaw, tick_ms);
        }
        break;
    }

    case EL_LOOP_OUT:
        s_o.e_bias   = 0.0f;
        s_o.yaw_hold = 1u;
        s_o.kp_scale = 1.0f;
        set_speed_target(s_v_corner);
        if (line && absf(err) < FSM_CENTER_ERR) {
            if (++s_relock >= FSM_RELOCK_CYCLES) {
                /* 一个环岛结束 → 下一个环岛按另一种类型处理（本图只有一个圆 + 一个六边形） */
                s_loop_kind = (s_loop_kind == LOOP_CIRCLE) ? LOOP_HEX : LOOP_CIRCLE;
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_relock = 0u;
        }
        /* 出口兜底：转够角度后最多再给 1 s 找回中线 */
        if (dwell > 1000u) {
            s_loop_kind = (s_loop_kind == LOOP_CIRCLE) ? LOOP_HEX : LOOP_CIRCLE;
            enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
        }
        break;

    /* ================= 其他 ================= */
    case EL_CROSS:
        /* 盲走：屏蔽 8 路位置项，靠 IMU 保持进线前的航向 */
        s_o.yaw_hold = 1u;
        s_o.kp_scale = 0.0f;
        set_speed_target(s_v_corner);
        if ((dist_mm - s_d_enter) > FSM_CROSS_BLIND_MM) {
            enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
        }
        break;

    case EL_CORNER:
        s_o.kp_scale = 1.2f;                   /* 弯道宁可多打一点 */
        s_o.yaw_hold = 0u;
        set_speed_target(s_v_corner);
        if (line && absf(err) < FSM_CENTER_ERR) {
            if (s_hold_since == 0u) s_hold_since = tick_ms;
            if (tick_ms - s_hold_since >= FSM_CENTER_HOLD_MS) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
            }
        } else {
            s_hold_since = 0u;
        }
        break;

    case EL_SCURVE:
        s_o.kp_scale = 0.75f;                  /* 弱化循迹，交给 yaw */
        s_o.yaw_hold = 1u;
        set_speed_target(CFG_V_SCURVE);
        if (!trk || (line && absf(err) < FSM_CENTER_ERR)) {
            if (s_hold_since == 0u) s_hold_since = tick_ms;
            if (tick_ms - s_hold_since >= FSM_SCURVE_HOLD_MS) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
            }
        } else {
            s_hold_since = 0u;
        }
        break;

    case EL_STOP:
    default:
        s_o.v_cmd = 0.0f;
        s_o.e_bias = 0.0f;
        s_o.kp_scale = 0.0f;
        s_o.yaw_hold = 0u;
        break;
    }

    /* 【TODO 接里程脚本】真正的比赛策略要给每个元素配一个里程窗：
     *   if (!in_window(dist_mm, EL_LOOP, 150)) 不允许进入环岛，
     * 用来把"环岛入口误判"和"盲走区误判"压到接近 0。
     * 现在先只用时间和传感器特征，因为里程标定要在实车上做。 */
}

/* ---------------- 运行时速度目标 ---------------- */

void fsm_set_speeds(float v_straight, float v_corner)
{
    if (v_straight > 0.0f) s_v_straight = v_straight;
    if (v_corner   > 0.0f) s_v_corner   = v_corner;
}

void fsm_get_speeds(float *v_straight, float *v_corner)
{
    if (v_straight) *v_straight = s_v_straight;
    if (v_corner)   *v_corner   = s_v_corner;
}

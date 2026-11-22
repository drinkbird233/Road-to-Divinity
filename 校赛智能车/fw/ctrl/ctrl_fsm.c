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
#include <math.h>

/* 由 ctrl_core 提供：补线的斜率（err/mm），用来区分弯道和断口 */
extern float ctrl_pred_slope(void);
extern float ctrl_pred_mabs(void);
extern uint8_t ctrl_vis_gap(void);
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
/* 2026-11-22：2 太松 —— 直角弯进弯前误差左右抖两下就被判成 S 弯
 *（实测三次里两次是 2→4→10，根本没进 TURN90）。真 S 弯翻转次数远不止 3 次。 */
#define FSM_OSC_FLIPS         3u
#define FSM_SCURVE_MAX_CNT    4u      /* S 弯时线上不会太多路；6 路连黑是拐角/路口 */
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

/* 盲走参数也能在线改（串口 B 命令）。宏只是上电默认值。
 * 为什么要在线改：这张图的断口长度一变就得重新编译，太慢；
 * 而且调盲走需要"600 → 1200 → 1830"这样一档一档试，编不过来人会疯。 */
static float     s_v_blind    = CFG_V_BLIND;
static int32_t   s_blind_max  = CFG_BLIND_MAX_MM;

/* 航向符号，全工程唯一来源（ctrl_core 的航向保持和定角转弯都读它）。
 * +1 = 右转 yaw 变大；-1 = 右转 yaw 变小。串口 Y 可在线翻转。 */
float g_yaw_sgn = CFG_YAW_SGN;

void fsm_toggle_yaw_sign(void)
{
    g_yaw_sgn = -g_yaw_sgn;
}

static uint32_t  s_t_enter;
static int32_t   s_d_enter;      /* 进入状态时的里程 */
static int32_t   s_d_blind;      /* 进入盲走时的里程 */
static int32_t   s_d_loop;       /* 进入环岛时的里程 */
static int32_t   s_d_run0;       /* 首次看到线时的里程：里程窗以此为原点 */
static uint8_t   s_gap_mileage;  /* 1 = 这次盲走是里程窗锁进来的（出口也要按里程放开） */
static int32_t   s_gap_d0 = CFG_GAP_WIN_D0_MM;  /* 断口里程窗下界（串口 G 可在线改） */
static int32_t   s_gap_d1 = CFG_GAP_WIN_D1_MM;  /* 上界；d1 <= d0 表示关闭 */
/* 为什么窗口要能在线改：**跑起来的里程和推着的里程不是一回事**。
 * 电机驱动下的打滑（尤其 8 个直角弯的刹车/原地转）会让 dist 比真实路径偏大，
 * 实测同一段路，手推 7235 的位置，跑起来读到 7692 还差 400~600 mm 才到。
 * 所以里程窗只能拿"跑"的数据标定，而且每改一次就要重编太慢 —— 做成在线可调。 */
static int32_t   s_force_blind_mm; /* >0 = 强制盲走预算（串口 Z），按距离结束、不看线 */
static uint8_t   s_blind_forced;   /* 1 = 当前这次盲走是 Z 强制的 */
static uint32_t  s_hold_since;
static uint32_t  s_t_junc;
static uint32_t  s_t_corner;
static uint32_t  s_line_ms;      /* 最后一次看到线的时刻（丢线超时保护用） */
static uint32_t  s_t_lost;
/* s_err_at_loss 已废弃：不再用"丢线前的误差幅值"区分弯道/断口
 *（实测同一处直角弯该值在 50~250 之间随机跳），改用"转到看见线"的行为判据。 */
static float     s_yaw_acc;      /* 连续累加的航向变化 */
static float     s_yaw_last;
static int8_t    s_sign[8];
static uint8_t   s_sign_n;
static uint32_t  s_flip_t[4];
static uint8_t   s_flip_n;
static uint8_t   s_relock;       /* 重新锁线计数 */
static uint8_t   s_armed;
static uint32_t  s_t_edge;       /* "线贴最边"持续多久，用来区分直角弯和环岛 */
static uint8_t   s_loop_kind;    /* LOOP_CIRCLE / LOOP_HEX */

/* ---- 直角弯 / 定角转弯（EL_TURN90） ---- */
static float     s_turn_target;  /* 目标航向 °，进弯时一次算好 */
static uint8_t   s_scan_back;  /* 扫过一圈没找到线，正在转回原朝向 */
static float     s_turn_start;   /* 进弯时的航向，用来算"已经转过多少度" */
static float     s_turn_dir;     /* +1 右转，-1 左转（由丢线前的误差符号决定） */
static float     s_err_lv;       /* 最后一次「有效」误差。
                                  * 丢线时 bsp_track 会把 err 清成 0，拿它判方向
                                  * 会永远判成"居中"，所以必须单独记一份。 */

static float wrap180f(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static float absf(float v) { return (v < 0.0f) ? -v : v; }

static void enter(element_id_t el, int32_t d, float yaw, uint32_t t)
{
    /* 离开盲走就把"里程窗锁定"清掉：下次从别的路径（视觉/补线/扫回）进盲走时，
     * 出口仍按原来的"看到线就退出"，不受里程约束。 */
    if (s_o.el == EL_BLIND && el != EL_BLIND) {
        s_gap_mileage  = 0u;
        s_blind_forced = 0u;
        s_force_blind_mm = 0;
    }
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

/* 进入直角弯：先按常规 enter()，再把"往哪转、转到多少"一次算好。
 *
 * 目标角用**绝对航向**表示（entry_yaw ± 90），不是"再转 90 度"的累计量。
 * 原因：yaw 只在 -180..180 之间，转过 ±180 边界时累计量会跳变；
 * 而绝对目标角每次都用 wrap180(target - yaw) 重新求差，天然免疫回绕。 */
static void enter_turn(element_id_t el, int32_t d, float yaw, uint32_t t, float dir)
{
    enter(el, d, yaw, t);
    s_turn_dir     = (dir >= 0.0f) ? 1.0f : -1.0f;
    s_scan_back    = 0u;
    s_turn_start   = yaw;               /* 记下起始航向，用来算"已经转过多少" */
    /* 目标角 = 当前航向 + 符号 * 方向 * 90。
     * 符号不可省：右转时 yaw 到底是变大还是变小取决于模块约定（g_yaw_sgn）。 */
    s_turn_target  = wrap180f(yaw + g_yaw_sgn * s_turn_dir * CFG_TURN_DEG);
    s_o.w_mode     = 1u;
    s_o.yaw_target = s_turn_target;
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
    s_yaw_acc = 0.0f;
    s_yaw_last = 0.0f;
    s_relock = 0u;
    s_loop_kind = LOOP_CIRCLE;
    s_d_blind = 0;
    s_d_loop = 0;
    s_turn_target = 0.0f;
    s_turn_dir = 1.0f;
    s_err_lv = 0.0f;
    s_o.w_mode = 0u;
    s_o.yaw_target = 0.0f;
}

void fsm_arm(void)
{
    s_armed = 1u;
    s_o.el = EL_START;
    s_o.v_cmd = 0.0f;
    s_o.e_bias = 0.0f;
    s_o.kp_scale = 1.0f;
    s_o.yaw_hold = 0u;
    s_o.w_mode = 0u;
    s_o.yaw_target = 0.0f;
    s_err_lv = 0.0f;
    s_turn_dir = 1.0f;
    s_flip_n = 0u;          /* 上一圈的翻转历史不能带进来，否则一起步就判 S 弯 */
    s_sign_n = 0u;
    s_gap_mileage = 0u;     /* 新的一圈：里程窗锁定标志必须清掉 */
    s_blind_forced = 0u;    /* 强制盲走也必须由新的 Z 命令重新设置 */
    s_force_blind_mm = 0;
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
    float gyro_abs = imu ? absf(imu->gyro_dps) : 0.0f;   /* 角速度大小，到位判据要用 */
    uint32_t dwell = tick_ms - s_t_enter;
    float err = trk ? trk->err : 0.0f;
    uint8_t line = (uint8_t)(trk && trk->valid && !trk->junction);
    uint8_t gap_ready;   /* EL_BLIND 出口门：里程没过窗口上界时不许退出 */

    (void)speed_avg;

    /* 角度环标志每个 tick 先清掉，只有 EL_TURN90 会把它置 1。
     * 放在这里统一清，而不是每个 case 里各清一次 —— 漏清一个状态就会永久卡在角度环。 */
    s_o.w_mode = 0u;
    s_o.yaw_hard = 0u;   /* 同上：只有确定没线的元素（断口/强制盲走）才会置 1 */

    /* 记录最后一次「有效」误差：丢线时 trk->err 已经被 bsp_track 清成 0，
     * 直接拿它判断"往哪边转"会永远得到 0，也就是永远判成居中丢线。 */
    if (line) s_err_lv = err;

    /* ===== 【强制盲走 2026-11-22 晚】串口 Z =====
     * 目的：把"盲走能不能走直"从里程标定里**彻底拆出来**单独验一次。
     * 不依赖窗口、不依赖丢线判定、不依赖赛道 —— 随时随地都能触发。
     *
     * 行为：立刻进 EL_BLIND，忽略线，走够 Z 指定的距离就停车。
     * 因为不看线，所以可以在光地上直接把车摆好，发 Z 1500，
     * 量它到底走了多直、多远 —— 这才是盲走本身的能力。
     *
     * 副作用（有意为之）：它不经过 EL_START，所以即使车下面没有线也能启动，
     * 不必先按 a。台架/空地验证就靠这一点。 */
    if (s_force_blind_mm > 0 && !s_blind_forced) {
        s_blind_forced = 1u;
        s_gap_mileage  = 0u;
        s_d_blind      = dist_mm;
        if (s_o.el != EL_BLIND) enter(EL_BLIND, dist_mm, yaw, tick_ms);
    }

    /* 【安全网】丢线太久就停车。
     * CFG_LOSS_AUTOPILOT=1 时，丢线后状态机不再做任何切换，
     * 靠补线（误差外推）自然分流：断口直行、弯道继续转。
     * 但万一两种情况都没成功（比如冲出赛道），必须有个兜底把它停下来，
     * 否则车会一直往前开。 */
    if (line) {
        s_line_ms = tick_ms;
    } else if (!s_blind_forced &&
               (uint32_t)(tick_ms - s_line_ms) > CFG_LOST_STOP_MS) {
        /* 强制盲走（Z）期间不适用这条：它本来就是"故意在没线的地方走"，
         * 而且有自己的距离预算当保险。不加这个豁免的话，光地上发 Z 会被
         * 这一拍立刻踢回 EL_STOP —— 表现为"命令有回显、车一动不动"（实测）。
         * 断口里程窗那次盲走不受影响：进断口前线刚刚丢，计时是新的。 */
        s_o.v_cmd = 0.0f;
        s_o.w_mode = 0u;
        enter(EL_STOP, dist_mm, yaw, tick_ms);
        return;
    }

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
        if (line) {
            /* 【里程窗原点】第一次真正看到线的里程 = 这一圈的 0 点。
             * 用相对里程而不是上电绝对里程：这样"车摆在哪、什么时候按 a"都不影响，
             * 换起点只要重新测一次窗口。 */
            s_d_run0 = dist_mm;
            enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
        }
        break;

    /* ================= 直道：所有元素的分发点 ================= */
    case EL_STRAIGHT:
        s_o.e_bias   = 0.0f;
        s_o.kp_scale = 1.0f;
        s_o.yaw_hold = 1u;
        set_speed_target(s_v_straight);

        if (!trk) break;

        /* ===== 【里程窗 · 入口 2026-11-22 晚】只看里程，不再要求"必须先丢线" =====
         *
         * 第一次实跑暴露的问题：车跑到 dist=8289（已深入断口 1054 mm），el 却一直停在 2，
         * 里程窗根本没执行。原因是 bsp_track 里 `valid = (cnt > 0)`——
         * 断口处只要**残留 1 路**被判成黑（胶带端口、反光、地面阴影），
         * valid 就还是 1，整个"丢线分支"（里程窗就写在里面）压根进不去。
         *
         * 既然断口在赛道上的里程是固定的，那就用里程直接触发，灰度只当辅助。
         * 安全性：EL_BLIND 里 kp_scale = 1（位置环照常工作），
         * 所以即使那个位置还有线，车也只是按 CFG_V_BLIND 继续跟线，不会跑偏；
         * 万一真的判早了，最坏结果是这一段慢一点，不会失控。
         *
         * 入口用 run < D1，出口用 run >= D1，同一个界分开，不会在边界上反复跳。 */
        if (s_gap_d1 > s_gap_d0) {
            int32_t run = dist_mm - s_d_run0;
            if (run >= s_gap_d0 && run < s_gap_d1) {
                s_gap_mileage = 1u;
                s_d_blind     = dist_mm;
                enter(EL_BLIND, dist_mm, yaw, tick_ms);
                break;
            }
        }

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
                /* ⚠️ 这里必须用 s_err_lv（最后一次有效值）。
                 * 原来写的是 err，而丢线时 err 已经被清成 0，
                 * 于是 absf(err) < 0.8 永远成立 —— 急弯丢线一律被当成"盲走区"，
                 * 进 EL_BLIND 后 kp_scale=0（位置环断开）→ 车头回正 → 直冲出去。
                 * 这是"弯道冲出去"的直接原因之一。 */
            }
            if (tick_ms - s_t_lost >= CFG_BLIND_ARM_MS) {

                /* ===== 【里程窗】断口位置固定，用"自起跑以来的里程"分流 =====
                 * 为什么要有这一条：直角弯和断口在 8 路上是同一件事（都是丢线），
                 * 实测 err 区间重叠、slope 都是 0，所有灰度判据都试过且失败。
                 * 断口在赛道上的位置是固定的，所以"跑到哪儿了"本身就是一个可靠信息源。
                 *
                 * 优先级最高：只要窗口有效（D1 > D0）就先按里程判，不再看灰度。
                 * D1 <= D0 时整个功能关闭，行为与原来完全一致。 */
                /* 【兜底】上面 EL_STRAIGHT 开头已经按里程无条件判过一次了；
                 * 这里是给"当时不在 STRAIGHT（比如还在 SEEK）"的情况留的后路。
                 * 用同一个运行时窗口，'G' 命令改一次两处都生效。 */
                if (s_gap_d1 > s_gap_d0) {
                    int32_t run = dist_mm - s_d_run0;
                    if (run >= s_gap_d0 && run < s_gap_d1) {
                        s_gap_mileage = 1u;
                        s_d_blind     = dist_mm;
                        enter(EL_BLIND, dist_mm, yaw, tick_ms);
                        break;
                    }
                }

                /* 【最终方案 2026-11-22 晚】不再做任何"分类"，也不进入任何新状态。
                 *
                 * 原理：线丢了以后，转向完全交给 ctrl_core 的"补线"（误差外推）。
                 *   · 断口：丢失前误差一直居中 → 外推≈0 → 车头不动 → 直行穿过 ✅
                 *   · 直角弯：丢失前误差在增大 → 外推饱和 → 车一直转，
                 *             转到看见新线为止（比固定 105° 更自适应）✅
                 *
                 * 好处：断口处**物理上不可能转**，因为根本没有误差信号可放大。
                 * 之前所有"分类"判据（err_lv / 斜率 / 视觉）都不稳定，
                 * 而"没有误差就不会转"是控制律本身的性质，不需要判断。
                 *
                 * 安全网：下面有全局的"丢线太久就停车"（CFG_LOST_STOP_MS）。 */
                if (CFG_LOSS_AUTOPILOT) {
                    break;              /* 什么都不做，让补线自己决定 */
                }

                /* ---- 以下为旧逻辑，CFG_LOSS_AUTOPILOT=0 时才走 ---- */
                if (ctrl_vis_gap()) {
                    s_d_blind = dist_mm;
                    enter(EL_BLIND, dist_mm, yaw, tick_ms);
                    break;
                }

                if (ctrl_pred_mabs() < CFG_GAP_MABS) {
                    s_d_blind = dist_mm;
                    enter(EL_BLIND, dist_mm, yaw, tick_ms);
                    break;
                }

                if ((CFG_GAP_STRAIGHT_MM > 0) &&
                    ((dist_mm - s_d_enter) >= CFG_GAP_STRAIGHT_MM)) {
                    s_d_blind = dist_mm;
                    enter(EL_BLIND, dist_mm, yaw, tick_ms);
                    break;
                }

                enter_turn(EL_TURN90, dist_mm, yaw, tick_ms,
                           (s_err_lv >= 0.0f) ? 1.0f : -1.0f);
                break;
            }
        } else {
            s_t_lost = 0u;
        }

        /* 3) 环岛入口：一侧重 + 另一侧仍有点
         *
         * 【2026-11-22 修】加"持续时间"判据。
         * 原来只要线贴到最边上就判环岛，结果**直角弯也被判成环岛**
         * （实测状态序列 2→6→7→10，车拐过去之后在环岛逻辑里超时停车）。
         *
         * 真实区别：
         *   直角弯：线贴边后**很快整片消失**（车错过了路口）
         *   环岛  ：线**一直贴在边上**，车跟着弯走
         * 所以要求"贴边"持续 CFG_LOOP_ARM_MS 才认定环岛；
         * 中途就丢线的，交给下面的丢线分支判成 TURN90。 */
        if (line && trk->count >= FSM_LOOP_SIDE_CNT &&
            ((trk->left_edge == 0 && trk->right_edge < 6) ||
             (trk->right_edge == 7 && trk->left_edge > 1))) {
            if (s_t_edge == 0u) s_t_edge = tick_ms;
            if ((uint32_t)(tick_ms - s_t_edge) >= CFG_LOOP_ARM_MS) {
                s_d_loop = dist_mm;
                s_yaw_acc = 0.0f;              /* 从这里开始累计转角 */
                s_o.e_bias = (err < 0.0f) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS;
                enter(EL_LOOP_IN, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_t_edge = 0u;
        }

        /* 4) S 弯：误差高频翻转
         *
         * 【2026-11-22 停用】实测这个判据纯属有害：
         *   · 三次直角弯测试里两次被判成 S 弯（2→4→10），根本没进转弯逻辑；
         *   · EL_SCURVE 里 yaw_hold=1，会跟"补线"抢方向盘，实测只转了 29°。
         * 而普通循迹本来就能过 S 弯，这个状态没有实际收益。
         * 需要时把 CFG_SCURVE_ENABLE 改回 1。 */
        if (CFG_SCURVE_ENABLE && line && trk->count <= FSM_SCURVE_MAX_CNT &&
            (osc_count(err, tick_ms) >= FSM_OSC_FLIPS)) {
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
        /* 【2026-11-22 改】不再屏蔽转向。
         * 原来 kp_scale=0，只靠航向保持直行。问题是"居中断口"和"弯道"的
         * 区分标准是**丢线前一刻的误差**，而这个值每次都不一样
         * （实测同一处直角弯丢线前 err_lv 在 50~250 之间跳），分类根本不可靠。
         * 判成 BLIND 就把方向盘关掉 → 直角弯到这里直接直着冲出去（实测只转 7°）。
         *
         * 既然有了补线，就不需要这个分类：
         *   断口：消失前斜率≈0 → 外推≈0 → 自然直行
         *   弯道：消失前斜率大   → 外推饱和 → 自然转弯
         * 所以把转向交回补线，航向保持仍开着做兜底（它只在误差小时起作用，
         * 不会跟补线抢方向）。 */
        s_o.kp_scale = 1.0f;
        s_o.e_bias   = 0.0f;
        s_o.yaw_hold = 1u;
        /* 这两种盲走都"确定没有线"：硬保持，不让假线把它屏蔽掉 */
        s_o.yaw_hard = (uint8_t)((s_blind_forced || s_gap_mileage) ? 1u : 0u);
        set_speed_target(s_v_blind);

        /* 【强制盲走 · 出口】Z 模式不看线，走够距离就停车。
         * 停而不是继续，是为了让你能下车量横向偏差 ——
         * 这是唯一能判断"航向保持到底行不行"的客观数据。 */
        if (s_blind_forced) {
            /* 让丢线超时计时保持新鲜：强制盲走结束后不会立刻被安全网停车，
             * 也避免测试过程中被这条通用保护打断。 */
            s_line_ms = tick_ms;
            /* 强制模式关掉位置环：要测的就是"航向保持本身"能不能把车走直，
             * 留着位置环的话，有线的地方就变成普通循迹了，测不出东西。 */
            s_o.kp_scale = 0.0f;
            if ((dist_mm - s_d_blind) >= s_force_blind_mm) {
                enter(EL_STOP, dist_mm, yaw, tick_ms);
                s_o.v_cmd = 0.0f;
            }
            break;
        }

        /* 【里程窗 · 出口】里程还没走过窗口上界，就不许退出这次盲走。
         * 断口处残留的那 1 路误判会让 line 变成 1；不拦住的话车会每 10 ms
         * 在 2 和 5 之间来回跳，而每次跳到 STRAIGHT 的那一拍，
         * 位置环就按那个假误差猛打一次方向 —— 这正是"盲走不走直线"的来源之一。
         * 只对"里程窗锁进来的那一次盲走"生效，其他路径进来的仍是原来"见线就退"。 */
        gap_ready = 1u;
        if (s_gap_mileage && (s_gap_d1 > s_gap_d0)) {
            gap_ready = (uint8_t)((dist_mm - s_d_run0) >= s_gap_d1);
        }

        if (line && gap_ready) {
            if (++s_relock >= FSM_RELOCK_CYCLES) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_relock = 0u;
        }

        /* 距离预算：超了就停车。这是防止"冲出赛道还一直往前开"的唯一保险 */
        if ((dist_mm - s_d_blind) > s_blind_max) {
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

        /* 在 EL_CORNER 里也会丢线：如果丢线前误差已经贴到视野边缘，
         * 说明这不是"跟不住的弯"而是"直角弯"，跟线在物理上已经不可能，
         * 必须切到定角转弯。要求连续丢线 ≥ Blind_ARM_MS 再动手，防抖动误判。 */
        if (!line) {
            if (s_t_lost == 0u) s_t_lost = tick_ms;
            if ((tick_ms - s_t_lost) >= CFG_BLIND_ARM_MS) {
                /* 和 EL_STRAIGHT 一样：CFG_LOSS_AUTOPILOT=1 时什么都不做，
                 * 由补线自己决定"直行还是继续转"。 */
                if (CFG_LOSS_AUTOPILOT) {
                    break;
                }
                if (ctrl_vis_gap()) {
                    s_d_blind = dist_mm;
                    enter(EL_BLIND, dist_mm, yaw, tick_ms);
                    break;
                }
                if (CFG_TURN90_ENABLE &&
                    (absf(s_err_lv) >= CFG_TURN_ARM_ERR)) {
                    enter_turn(EL_TURN90, dist_mm, yaw, tick_ms, s_err_lv);
                    break;
                }
            }
        } else {
            s_t_lost = 0u;
        }

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

    /* ================= 直角弯：陀螺仪定角转弯 =================
     * 不用 8 路位置项（kp_scale=0），改为 w_mode=1，让 ctrl_core 用
     *   w_ref = TURN_KP * wrap180(yaw_target - yaw)
     * 直接给角速度目标。速度降到 CFG_V_TURN，让转弯半径尽量小。
     * 到位判据用"|dh| < TOL 且连续保持 SETTLE_MS"，单看瞬时值会被过冲骗过去。 */
    case EL_TURN90: {
        float dh = wrap180f(s_o.yaw_target - yaw);

        /* 【刹车相】进弯后先停车一小段，把速度从行进速度压到 0，再开始转。
         *
         * 为什么必须这样：进弯瞬间车还在 300 mm/s，而转弯指令要求两轮立刻
         * 变成 (275, -195)，右轮要跳变 495 mm/s。电机 + PID 跟不上，
         * 过渡期间车带着高速往前冲 —— 弧线变大、位置偏移，转完就找不到线。
         * 而且这个过渡时间每次都不一样（受 PID 积分状态、抓地、电压影响），
         * 所以表现为"有时候转得很准，有时候慢悠悠冲过头"。
         *
         * 加一个固定的刹车时间，让每次转弯都从 v≈0 开始，行为就一致了。 */
        if ((uint32_t)(tick_ms - s_t_enter) < CFG_TURN_BRAKE_MS) {
            s_o.w_mode   = 0u;      /* 不转 */
            s_o.kp_scale = 0.0f;    /* 也不用位置环 */
            set_speed_target(0.0f); /* 刹停 */
            break;
        }

        /* 【最后一道保险】刹车相期间车已经停下来了，此时再问一次视觉：
         * 如果它说"前面没线"，那就是断口而不是直角弯 —— 车还没开始转，
         * 航向没损失，直接改盲走最干净。
         * 这一步覆盖了"丢线时视觉的 GAP 还没报上来、先把转弯启动"的情况。 */
        if (ctrl_vis_gap()) {
            s_o.w_mode = 0u;
            s_d_blind  = dist_mm;
            enter(EL_BLIND, dist_mm, yaw, tick_ms);
            break;
        }

        s_o.kp_scale   = 0.0f;
        s_o.e_bias     = 0.0f;
        s_o.yaw_hold   = 0u;
        s_o.w_mode     = 1u;
        s_o.yaw_target = s_scan_back ? s_turn_start : s_turn_target;
        set_speed_target(CFG_V_TURN);

        /* 到位判据：角度进容差 **而且** 角速度已经降下来。
         * 只看角度的话，车还在以 200 °/s 冲过目标时 |dh| 也会短暂落在容差内，
         * 判据立刻成立 -> 停下时已经多转好几度（u 90 连转四次累积偏就是这个）。 */
        if ((absf(dh) < CFG_TURN_TOL_DEG) && (gyro_abs < CFG_TURN_STOP_DPS)) {
            if (s_hold_since == 0u) s_hold_since = tick_ms;
            if ((tick_ms - s_hold_since) >= CFG_TURN_SETTLE_MS) {
                s_o.w_mode = 0u;
                if (s_scan_back) {
                    /* 找了一圈没找到线、已经转回原朝向 → 确实是断口，直行穿过 */
                    s_scan_back = 0u;
                    s_d_blind   = dist_mm;
                    enter(EL_BLIND, dist_mm, yaw, tick_ms);
                } else {
                    enter(EL_SEEK, dist_mm, yaw, tick_ms);
                }
            }
        } else {
            s_hold_since = 0u;
        }

        /* 自适应提前收：赛道上的弯并不都是正好 90°（左上有一小段 45° 斜切）。
         * 转角过程中只要重新看到线、而且误差已经很小，就说明已经对正，不必转满。
         * 要求连续 2 拍成立，避免把路过的一根反光线当成"找到了"。
         *
         * 【2026-11-22 修】必须加"已转过多少度"的门槛。
         * 否则旋转过程中传感器**一定会扫过原来那条线**，那一瞬间 |err| 很小，
         * 就被判成"已对正"提前退出 —— 实测直角弯只转了 58° 就退出去，
         * 然后 SEEK 直行 144 mm 找不到线，停车。
         * 门槛取 CFG_TURN_EARLY_MIN_DEG（默认 45°），
         * 45° 斜切仍然能在转过 45° 之后被正确识别。 */
        if (line && absf(err) < CFG_TURN_FIND_ERR &&
            absf(wrap180f(yaw - s_turn_start)) >= CFG_TURN_EARLY_MIN_DEG) {
            if (++s_relock >= FSM_RELOCK_CYCLES) {
                s_o.w_mode = 0u;
                enter(EL_SEEK, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_relock = 0u;
        }

        /* 【行为区分的关键】转满 CFG_TURN_SCAN_MAX 还没看到线 → 这不是弯道，是断口。
         * 此时**不能**就地改直行（车头已经歪了 110°，直行等于斜着冲出赛道）。
         * 正确做法：掉头转回进弯时的航向，回到原方向后再直行穿过断口。
         * 代价是多花约 1 秒，但保证盲走区的朝向是正的。 */
        if (!line && !s_scan_back &&
            (absf(wrap180f(yaw - s_turn_start)) >= CFG_TURN_SCAN_MAX)) {
            s_scan_back = 1u;
            s_hold_since = 0u;
            s_t_enter = tick_ms;        /* 重新计时，给回转留够时间 */
        }

        /* 超时兜底：转不过来（打滑、卡住、目标角算错）也不能原地打转 */
        if ((tick_ms - s_t_enter) > CFG_TURN_TIMEOUT_MS) {
            s_o.w_mode = 0u;
            enter(EL_SEEK, dist_mm, yaw, tick_ms);
        }
        break;
    }

    /* ================= 转完找线 =================
     * 转完 90° 之后线在哪里是未知的（可能就在车下，也可能要往前蹭几厘米），
     * 所以低速直行 + 打开循迹，连续 2 拍看到线才算重新锁定。 */
    case EL_SEEK:
        s_o.kp_scale = 1.0f;
        s_o.e_bias   = 0.0f;
        s_o.yaw_hold = 0u;
        set_speed_target(CFG_V_SEEK);

        /* 【2026-11-22 修】SEEK 里也要看视觉的"断口"预告。
         * 实测：盲走区紧跟在两个急转弯后面，车转完 105° 进 SEEK，
         * 前面本来就没有线，SEEK 找 800 ms 找不到 → 超时 → STOP → 停车。
         * 只要视觉说"前面没线"，就直接进盲走，让陀螺仪保持航向穿过去。 */
        if (ctrl_vis_gap()) {
            s_d_blind = dist_mm;
            enter(EL_BLIND, dist_mm, yaw, tick_ms);
            break;
        }

        if (line) {
            if (++s_relock >= FSM_RELOCK_CYCLES) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_relock = 0u;
        }

        if (dwell > CFG_SEEK_TIMEOUT_MS) {
            enter(EL_STOP, dist_mm, yaw, tick_ms);
            s_o.v_cmd = 0.0f;
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

/* ---------------- 盲走参数（串口 B 命令） ----------------
 * 传 <=0 表示"这一项不改"，方便只改距离或只改速度。 */
void fsm_set_blind(int32_t max_mm, float v)
{
    if (max_mm > 0)   s_blind_max = max_mm;
    if (v > 0.0f)     s_v_blind   = v;
}

void fsm_get_blind(int32_t *max_mm, float *v)
{
    if (max_mm) *max_mm = s_blind_max;
    if (v)      *v      = s_v_blind;
}

/* 断口里程窗（serial 'G' 命令）。d1 <= d0 表示关闭。 */
void fsm_set_gap(int32_t d0, int32_t d1)
{
    s_gap_d0 = d0;
    s_gap_d1 = d1;
    s_gap_mileage = 0u;
}

void fsm_get_gap(int32_t *d0, int32_t *d1)
{
    if (d0) *d0 = s_gap_d0;
    if (d1) *d1 = s_gap_d1;
}

/* 强制盲走（serial 'Z' 命令）。mm <= 0 表示取消。 */
void fsm_force_blind(int32_t mm)
{
    s_force_blind_mm = (mm > 0) ? mm : 0;
    s_blind_forced   = 0u;
}

int32_t fsm_force_blind_left(void)
{
    return s_force_blind_mm;
}

/* 最后一次有效线误差（丢线后仍保留） */
float fsm_err_lv(void) { return s_err_lv; }

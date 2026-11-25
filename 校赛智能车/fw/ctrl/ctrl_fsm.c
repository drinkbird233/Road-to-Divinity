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
/* 定角转弯的在线参数（T 命令改，定义在 ctrl_core.c） */
extern float g_turn_deg;
extern float g_turn_v;
extern float g_turn_brake;
/* 开环差速转弯（O 命令改） */
extern float g_turn_mode;
extern float g_turn_open_dv;
extern float g_turn_open_ms;
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
#define FSM_LOOP_SIDE_CNT     5u      /* 一侧 ≥5 路连黑算"侧重"，判环岛入口。
                                       * 【2026-11-24：4 → 5】实测 4 路不够：
                                       * 车只是偏到一边（raw=F0 → mask=F0，压线 4~7，
                                       * 也就是"最右 4 路"）就会被当成环岛入口 ——
                                       * 19:52 那趟在 dist=8589 就是这样误进了环岛。
                                       * 真正的环岛分叉是 6 路连黑（mask=FC，压线 2~7），
                                       * 而且是从边缘一直连到中间。 */
#define FSM_RELOCK_CYCLES     2u      /* 盲走/路口出口要求连续 N 拍看到线才算重新锁线 */

typedef struct {
    float    v;             /* 绕行速度 */
    float    exit_deg;      /* 累计转角到多少度算绕完 */
    uint32_t timeout_ms;    /* 超时兜底，防止卡死在环里 */
} loop_cfg_t;

/* 环岛速度改成**运行时可调**（串口 S 命令）。
 * 原来速度写死在 const 表里，想调一格就得重编译 —— 一次实验要烧一版，代价太大。 */
static float s_v_loop[2];

static const loop_cfg_t s_loop_tbl[2] = {
    { CFG_V_LOOP_CIRCLE, 330.0f, 4000u },   /* LOOP_CIRCLE：圆形环岛 */
    /* 六边形要绕满 360°：按边长 300 mm、周长约 1.8 m、180 mm/s 算需要约 10 s，
     * 原来的 6 s 会在绕完之前就把车踢出去（表现为"环岛里走一半就直冲"）。
     * 这里的超时只是兜底，正常情况下由 exit_deg=360° 收尾。 */
    { CFG_V_LOOP_HEX,    360.0f, 12000u }
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
static uint32_t  s_t_edge_last;  /* 环岛入口形态最近一次命中的时刻（2026-11-24） */
static uint8_t   s_loop_kind;    /* LOOP_CIRCLE / LOOP_HEX */
/* 环岛入口该往哪边拐：+1 右 / -1 左。
 * 由入口那一拍"哪一侧贴边"决定（right_edge==7 → 右）。
 * 【2026-11-24】之前没有这个量，入口段靠 (err<0) 的符号临时推，
 * 而顶点处 err≈0，符号是随机的 —— 实测就是直着过去的直接原因。 */
static int8_t    s_loop_dir = 1;

/* ---- 直角弯 / 定角转弯（EL_TURN90） ---- */
static float     s_turn_target;  /* 目标航向 °，进弯时一次算好 */
static uint8_t   s_scan_back;  /* 扫过一圈没找到线，正在转回原朝向 */
static float     s_turn_start;   /* 进弯时的航向，用来算"已经转过多少度" */
static float     s_turn_dir;     /* +1 右转，-1 左转（由丢线前的误差符号决定） */
static float     s_err_lv;       /* 最后一次「有效」误差。
                                  * 丢线时 bsp_track 会把 err 清成 0，拿它判方向
                                  * 会永远判成"居中"，所以必须单独记一份。 */

/* ---- 提前入弯（2026-11-24 新增，见 docs/20_moving_turn_plan.md） ----
 * 不等丢线，线偏到倒数第二路就开始按角度转。三个状态量分工：
 *   s_pre_armed —— "上膛"：线居中过才置 1，进任何非 STRAIGHT 元素就清。
 *   s_pre_run / s_t_pre —— 超阈值的持续计时（30 ms）。
 *   s_turn_pre  —— 本次 TURN90 是提前入弯进来的（决定"中止"和"出弯去哪"）。 */
static float     s_pre_err  = CFG_PRE_TURN_ERR;               /* P 命令在线改 */
static float     s_pre_hold = (float)CFG_PRE_TURN_HOLD_MS;
static uint8_t   s_pre_on   = CFG_PRE_TURN_ENABLE;
static uint8_t   s_pre_armed;
static uint8_t   s_pre_run;
static uint32_t  s_t_pre;
static uint32_t  s_t_cool;       /* 离开 TURN90 的时刻，用于冷却 */
static uint8_t   s_turn_pre;     /* 1 = 本次 TURN90 由提前入弯触发 */
static uint8_t   s_pre_ok;       /* 中止判据的连续成立拍数 */
/* 1 = 本次提前入弯是"线变宽（cnt>=CFG_PRE_TURN_CNT）"触发的。
 * 为什么需要区分：宽黑是直角弯入口的形状（实测 raw=E0/E0 → 压线 5~7 路），
 * 这种触发说明车正压在拐点上，**不该再走"45 度斜切"的中止判据** ——
 * 中止判据会在车还在拐点上、传感器还看得见来路那条线时把转弯取消。
 * 只有"线偏到边缘"（|err| 大）触发的，才可能是 45° 斜切，才需要中止。 */
static uint8_t   s_pre_wide;
/* 触发那一拍的形态快照（调试用，日志 [turn] ... cnt= e= 打出来） */
static int32_t   s_d_loop_exit = -100000;  /* 上一次出环岛的里程（连进冷却用） */
static float     s_loop_yaw0;              /* 判进环岛那一刻的航向（强制转弯的基准） */
static uint8_t   s_loop_done;              /* 1 = 本圈已经绕过环岛（之后不许再判直角弯） */
static uint8_t   s_trig_cnt;
static int16_t   s_trig_err100;
/* 上一次转完时的里程：用来做"转完冷却"。刚转完就再次丢线不是新弯，
 * 是残余误差（实测 ±3.0），再转一次就成了 200° 以上的掉头。 */
static int32_t   s_d_turn_exit = -100000;

/* ---- 转弯计数 → 自动进盲走（2026-11-23 新增） ----
 * 赛道结构：一圈 7 个直角弯，最后两个是连续右转，之后就是 1.5 m 的断口盲走区。
 * 所以"数到第 7 个弯"是比任何灰度判据都可靠的"该进盲走了"的信号。
 *
 * 为什么用计数而不是里程：里程会因打滑累积 6% 误差（实测手推 7235 处，
 * 跑起来要到 7692），而转弯次数是离散的、不会漂。 */
static uint8_t   s_turn_cnt;     /* 已经转完的直角弯个数 */
static uint8_t   s_blind_done;   /* 计数触发的盲走是否已经用掉（只触发一次） */
/* 调试用：假装"盲走已经走完"（串口 K 1）。
 * 用途：把车直接放到"盲走出口、刚接上线"的位置单独练环岛，
 * 一次只要 10 秒，不用跑整圈 55 秒。
 * 不能复用 s_blind_done —— 它在 fsm_arm() 里会被清 0（发车时清），
 * 那样"先发 K 1 再按 a"就失效了。 */
static uint8_t   s_dbg_postblind;
static uint8_t   s_blind_lock;   /* 1 = 这次盲走写死航向（用发车方向+180） */
static uint8_t   s_finish_armed; /* 1 = 已经过盲走/环岛，再遇到全黑就是终点 */
static uint32_t  s_t_finish;     /* 全黑已持续多久，用来确认是终点而不是噪声 */
static float     s_start_yaw;    /* 发车时的航向（仅 CFG_BLIND_USE_ABS_YAW=1 时用） */
/* 进盲走那一刻的航向 = 断口那条胶带的实际走向。
 * 【2026-11-24 修】原来盲走用绝对值 wrap180(发车航向 + 180°)，实测差 15°：
 *   断口处胶带走向 192.7°（进盲走时 yaw=-167.3、且 err=0，车是正的），
 *   而"发车航向+180"算出来是 177.3°。
 *   车在盲走里被硬拧 15°，再斜着走 1.9 m → 横偏约 500 mm，线永远撞不到
 *   （实测 8075 进、一路 raw=FF 到 9976 都没看见线，最后 10 s 超时停车）。
 * 前提：这图底部那段和顶部起跑段并不平行，所以"绝对值"这个假设不成立。 */
static float     s_blind_yaw0;
static int8_t    s_dir_h1;       /* 最近一次转弯方向：+1 右 / -1 左 */
static int8_t    s_dir_h2;       /* 上一次转弯方向 */

/* 进盲走的"模式"判据：
 *   已转弯数 >= s_blind_min_cnt  且  最近两次转弯方向都 == s_blind_req_dir
 * 比"固定第 7 个"好在：漏识别一两个弯也能触发，只要最后两个是右右。
 * 两个都用 float/int 存，方便 C 命令在线改；min<=0 表示关闭。 */
static float     s_blind_min_cnt = (float)CFG_BLIND_MIN_TURNS;
static int8_t    s_blind_req_dir = (int8_t)CFG_BLIND_REQ_DIR;

/* 盲走里程窗（串口 M 命令改）。max <= min 表示不检查里程。
 * 原点用 s_d_run0（第一次看到线那一刻），与断口里程窗 s_gap_d0/d1 同一套原点。 */
static int32_t   s_blind_win_min = CFG_BLIND_RUN_MIN;
static int32_t   s_blind_win_max = CFG_BLIND_RUN_MAX;

/* ---- 出盲走"锁存形状"（2026-11-24 新增，docs/22 的 P0）----
 * 实测：出断口那一刻只有 2 路压线（raw=FC → mask=0xC0 → 最右两路），
 * 而环岛形状判据要 3 路。于是 side_loop=0 → 回 EL_STRAIGHT；
 * 16 mm 之后才涨到 3 路（raw=F8），但车已经在直道里了 —— [el] 6 从不出现。
 * 所以判定不能只看"出口那一拍"，要在出口窗口内锁存"见过什么形状"。 */
static uint8_t   s_loop_seen;      /* 1 = 本段盲走出口处见过环岛入口形状 */
static float     s_loop_bias;      /* 锁存到的偏置方向（-1 左 / +1 右） */
static int32_t   s_d_firstline = -1; /* 出盲走第一次看到线的里程 */
/* 1 = 这次盲走是"线还在时"被里程窗抢进来的，必须先真的丢一次线才允许退出。
 * 【2026-11-24 修】19:52 那趟：窗口在 7963 抢进盲走，可胶带到 8050 才断，
 * 于是在盲走里又看见线、60 mm 后就退出去了 —— 退出后车正好压在断口起点，
 * 连做两个假转弯，整趟崩掉。这条标志把"抢跑"变成"等真丢线"。 */
static uint8_t   s_blind_needlost;
/* 丢线确认时间（ms），串口 R 命令在线改。
 * 【2026-11-24】原来固定 120 ms。实测从"最后看见线"到 EL_TURN90 建好，
 * 车已经冲出去 65 mm（300 mm/s × 200 ms ≈ 60 mm），用户反馈"反应太慢"。
 * 现在分两档：
 *   线是**偏着丢的**（|err_lv| ≥ CFG_TURN_ARM_ERR） → 直角弯特征，用快的；
 *   线是**居中丢的** → 可能是断口/反光，用慢的，别误判。
 * 两者互不影响，所以快档可以压得很小而不牺牲断口识别。 */
static uint16_t  s_confirm_fast = CFG_TURN_CONFIRM_FAST_MS;
static uint16_t  s_confirm_slow = CFG_BLIND_ARM_MS;

static float wrap180f(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static float absf(float v) { return (v < 0.0f) ? -v : v; }

/* ===== 转弯方向（2026-11-24 新增）=====
 * 原来方向完全靠"丢线前最后一次有效误差"的符号推：err>0 = 线在右 = 右转。
 * 实测这个判据会被"转完出来的航向偏差"污染：
 *   转完虽然 err≈0（传感器压在线上），但车身还斜着十几度，
 *   跑 100 mm 就横向漂到线另一侧，err 变成 +3.0，
 *   到下一个弯丢线时 err_lv=+350 → 判成右转 → 整车掉头。
 * 正方形/固定方向的赛道根本没有"猜"的必要，所以给一个方向锁：
 *   s_dir_force = +1 强制右转，-1 强制左转，0 才回退到看 err 符号。
 * 现场用 D 命令切换（掉头换了方向就发一条 D -1 / D 1）。 */
static int8_t    s_dir_force = (int8_t)CFG_TURN_FORCE_DIR;

static float turn_dir_from_err(void)
{
    if (s_dir_force > 0) return  1.0f;
    if (s_dir_force < 0) return -1.0f;
    return (s_err_lv >= 0.0f) ? 1.0f : -1.0f;
}

/* 直角弯方向（宽黑触发时用）。
 * 宽黑那一帧 err 往往在 0 附近（实测 err=+1），符号不可靠；而"宽黑朝哪边多"
 * 才是弯往哪边拐的直接证据：右转时出去的胶带向右侧延伸，实测 raw=E0 → mask=F8
 * → 压线 5,6,7（右边缘到边）。
 * 优先级：D 方向锁 > |err| 明显（>=0.5）> 形状 > err 符号。 */
static float turn_dir_corner(int cnt, int le, int re, float err)
{
    if (s_dir_force > 0) return  1.0f;
    if (s_dir_force < 0) return -1.0f;
    if (absf(err) >= 0.5f) return (err >= 0.0f) ? 1.0f : -1.0f;
    if (cnt >= (int)CFG_PRE_TURN_CNT) {
        if (re >= 6) return  1.0f;
        if (le <= 1) return -1.0f;
    }
    return (err >= 0.0f) ? 1.0f : -1.0f;
}

/* ===== 断口判据（docs/21 三重同意 + 里程兜底，2026-11-24 实车改成"或"）=====
 * 抽成函数是因为它现在有**三个调用点**：
 *   ① EL_STRAIGHT 丢线、② EL_CORNER 丢线、③ STRAIGHT 里"线只在最边缘"。
 * 原来只写在 ①，所以车一旦被判进 EL_CORNER 再丢线，盲走**永远不会触发**
 * —— 实测就这样在断口前爬了 10.5 s 然后停车（见 blind_want 的调用点注释）。 */
static uint8_t blind_win_on(void)
{
    return (uint8_t)((s_blind_win_max > s_blind_win_min) ? 1u : 0u);
}

static uint8_t blind_win_hit(int32_t d)
{
    int32_t run = d - s_d_run0;
    if (!blind_win_on()) return 0u;
    return (uint8_t)((run >= s_blind_win_min && run <= s_blind_win_max) ? 1u : 0u);
}

/* ① 转弯次数（min_cnt < 0.5 表示该条件被关闭） */
static uint8_t blind_cnt_ok(void)
{
    return (uint8_t)((s_blind_min_cnt >= 0.5f &&
                      (float)s_turn_cnt >= s_blind_min_cnt) ? 1u : 0u);
}

/* ② 最近两次转弯方向。
 * ⚠️ 方向锁（D）打开时不能比 s_dir_h1/h2：锁成左转后历史恒为 [-1,-1]，
 * 而 REQ_DIR 常配 +1(右) → seq_ok 恒 0 → 盲走**静默永不触发**。 */
static uint8_t blind_seq_ok(void)
{
    if (s_dir_force != 0) {
        return (uint8_t)((s_blind_req_dir == 0 ||
                          s_blind_req_dir == s_dir_force) ? 1u : 0u);
    }
    if (s_blind_req_dir != 0) {
        return (uint8_t)((s_dir_h1 == s_blind_req_dir &&
                          s_dir_h2 == s_blind_req_dir) ? 1u : 0u);
    }
    return 1u;
}

/* 该不该进盲走：计数同意 或 里程兜底（任务书 §6.3 也写了"谁先满足都行"）。
 * 不取"与"的原因见下面 EL_STRAIGHT 里的长注释。 */
static uint8_t blind_want(int32_t d)
{
    if (s_blind_done) return 0u;
    if (blind_cnt_ok() && blind_seq_ok()) return 1u;
    if (blind_win_hit(d))                 return 1u;
    return 0u;
}

/* 前向声明。enter() 的定义在下面（原文件顺序如此），而 blind_go() 要用它：
 * 不声明的话 ARMCC 会先按隐式 int enter(...) 处理，然后在真正定义处报
 *   #159 declaration is incompatible with previous "enter"
 * 这一类错误只在"新加的函数早于被调函数"时出现，C89 的隐式声明规则。 */
static void enter(element_id_t el, int32_t d, float yaw, uint32_t t);

/* 真正进盲走：置标志 + 写死航向。
 * ⚠️ 必须在这里就把标志写进 s_o：enter() 之后立刻 break，
 * 本 tick 不会执行 case EL_BLIND，而 ctrl_core 就是在"元素切换那一拍"
 * 读 blind_lock 决定航向参考用绝对角还是当前角。 */
static void blind_go(int32_t d, float yaw, uint32_t t, uint8_t needlost)
{
    s_blind_done     = 1u;
    s_blind_lock     = 1u;
    s_blind_needlost = needlost;
    s_d_blind        = d;
    s_o.blind_lock = 1u;
    s_o.yaw_hard   = 1u;
    s_o.kp_scale   = 0.0f;
    s_blind_yaw0   = yaw;                 /* 断口胶带的实际走向 */
    s_o.yaw_target = (CFG_BLIND_USE_ABS_YAW)
                     ? wrap180f(s_start_yaw + CFG_BLIND_YAW_OFFSET)
                     : s_blind_yaw0;
    enter(EL_BLIND, d, yaw, t);
}

static void enter(element_id_t el, int32_t d, float yaw, uint32_t t)
{
    /* 【提前入弯】"上膛"只在 STRAIGHT 里维护，进任何其它元素都清掉。
     * 不清的话，刚出弯、线还在视野边缘（cnt=1、err=3.0）那几拍会被当成
     * "前方有直角弯"，车会连转两个 90°。 */
    if (el != EL_STRAIGHT) s_pre_armed = 0u;
    /* 进盲走：形状锁存清零，等出口重新积累 */
    if (el == EL_BLIND) {
        s_loop_seen   = 0u;
        s_loop_bias   = 0.0f;
        s_d_firstline = -1;
    }
    /* 离开直角弯：记录冷却起点，并清掉"这是提前入弯"的标记
     * （否则下一次从别的路径进来的 TURN90 会继承这个标记，走错的出弯分支） */
    /* 离开环岛出口段：记里程，供"环岛不可连进"冷却用。
     * 实测（run_222759）六边形连绕了 3 圈：出环后 36 mm 就再次判进入口，
     * 因为**进环和出环都在同一个岔口**，出口必然又从宽黑上压过去。
     * 不冷却就是死循环，一圈白烧 40 s。 */
    if (s_o.el == EL_LOOP_OUT && el != EL_LOOP_OUT) {
        s_d_loop_exit = d;
        s_loop_done   = 1u;     /* 本圈绕过环岛了：之后不再允许"丢线→转 90°" */
    }
    if (s_o.el == EL_TURN90 && el != EL_TURN90) {
        s_t_cool      = t;
        s_d_turn_exit = d;      /* 转完冷却的起点（毫米） */
        s_turn_pre    = 0u;
        s_pre_ok      = 0u;
    }
    /* 离开盲走就把"里程窗锁定"清掉：下次从别的路径（视觉/补线/扫回）进盲走时，
     * 出口仍按原来的"看到线就退出"，不受里程约束。 */
    if (s_o.el == EL_BLIND && el != EL_BLIND) {
        s_gap_mileage  = 0u;
        s_blind_forced = 0u;
        s_force_blind_mm = 0;
        s_blind_lock   = 0u;   /* 写死航向只对"那一次"盲走有效 */
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
    /* 数弯 + 记方向历史：每次真的进 EL_TURN90 才计一次
     * （EL_CORNER 和 EL_STRAIGHT 两条入口都经过这里）。 */
    if (s_turn_cnt < 250u) s_turn_cnt++;
    s_dir_h2 = s_dir_h1;
    s_dir_h1 = (dir >= 0.0f) ? 1 : -1;
    s_turn_dir     = (dir >= 0.0f) ? 1.0f : -1.0f;
    s_scan_back    = 0u;
    /* 默认按"丢线才转"处理；提前入弯那条入口会在调用之后把它置回 1。
     * 必须显式清：不清的话，上一次提前入弯的标记会被"丢线转"继承，
     * 于是走错出弯分支（直接回 STRAIGHT、跳过 SEEK）。 */
    s_turn_pre     = 0u;
    s_pre_ok       = 0u;
    s_pre_wide     = 0u;
    s_trig_cnt     = 0u;        /* 0 = 不是提前入弯触发的（日志里 pre=0 时 c=0） */
    s_trig_err100  = 0;
    s_turn_start   = yaw;               /* 记下起始航向，用来算"已经转过多少" */
    /* 目标角 = 当前航向 + 符号 * 方向 * 90。
     * 符号不可省：右转时 yaw 到底是变大还是变小取决于模块约定（g_yaw_sgn）。 */
    s_turn_target  = wrap180f(yaw + g_yaw_sgn * s_turn_dir * g_turn_deg);
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
    s_loop_dir  = 1;
    s_d_loop_exit = -100000;
    s_loop_done   = 0u;
    s_loop_yaw0   = 0.0f;
    s_v_loop[LOOP_CIRCLE] = CFG_V_LOOP_CIRCLE;
    s_v_loop[LOOP_HEX]    = CFG_V_LOOP_HEX;
    s_d_blind = 0;
    s_d_loop = 0;
    s_turn_target = 0.0f;
    s_turn_dir = 1.0f;
    s_err_lv = 0.0f;
    s_o.w_mode = 0u;
    s_o.yaw_target = 0.0f;
    s_o.blind_lock = 0u;
    s_pre_armed = 0u;
    s_pre_run   = 0u;
    s_t_pre     = 0u;
    s_t_cool    = 0u;
    s_turn_pre  = 0u;
    s_pre_ok    = 0u;
    s_pre_wide  = 0u;
    s_d_turn_exit = -100000;
    s_turn_cnt   = 0u;
    s_blind_done = 0u;
    s_blind_lock = 0u;
    s_start_yaw  = 0.0f;
    s_dir_h1     = 0;
    s_dir_h2     = 0;
    s_finish_armed = 0u;
    s_t_finish     = 0u;
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
    s_pre_armed = 0u;       /* 新的一圈：重新"上膛"（等线居中） */
    s_pre_run   = 0u;
    s_t_pre     = 0u;
    s_turn_pre  = 0u;
    s_pre_ok    = 0u;
    s_pre_wide  = 0u;
    s_blind_needlost = 0u;
    s_t_edge      = 0u;     /* 环岛入口的贴边计时，不能带进新的一圈 */
    s_t_edge_last = 0u;
    s_flip_n = 0u;          /* 上一圈的翻转历史不能带进来，否则一起步就判 S 弯 */
    s_sign_n = 0u;
    s_gap_mileage = 0u;     /* 新的一圈：里程窗锁定标志必须清掉 */
    s_blind_forced = 0u;    /* 强制盲走也必须由新的 Z 命令重新设置 */
    s_force_blind_mm = 0;
    s_turn_cnt   = 0u;      /* 新的一圈重新数直角弯 */
    s_d_turn_exit = -100000;/* 新的一圈：没有"刚转完"的冷却 */
    s_blind_done = 0u;
    s_blind_lock = 0u;
    s_dir_h1     = 0;
    s_dir_h2     = 0;
    s_finish_armed = 0u;
    s_t_finish     = 0u;
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
    s_o.blind_lock = 0u; /* 同上：只有"数够 N 个弯"触发的那次盲走才置 1 */

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
            /* 【发车航向】盲走的绝对目标 = 它 + CFG_BLIND_YAW_OFFSET。
             * 在这里记而不是 fsm_arm()：arm 只是按下启动键，车可能还没摆正；
             * 而"看到线"这一刻车已经在起跑线上、方向是最终的。 */
            s_start_yaw = yaw;
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

        /* 【提前入弯 · 上膛】线居中过 => 才允许"不等丢线就转"。
         *
         * 为什么需要这一步（原方案没有，实车会毁圈）：
         * 刚出弯回 STRAIGHT 的那几拍 err 就在 3.0 附近（线在视野最外沿），
         * 拿它直接判"前方是直角弯"，车会接着再转一个 90°。
         * 而真实直角弯前面必然有一段线居中的直道，所以"居中过"是个
         * 廉价又可靠的开门条件 —— 它不是判弯，只是判"现在有资格判弯"。 */
        if (line && (absf(err) <= CFG_PRE_TURN_ARM_ERR)) s_pre_armed = 1u;

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

        /* ===== 【盲走里程窗 · M 命令】入口只看里程，不看线 =====
         *
         * 和上面的 G 窗口完全同一套语义，只是数据源换成三判据用的 s_blind_win_*。
         * 为什么必须"不看线"（19:27 那趟实测）：
         *   断口前 dist=7966 处残留 2 路胶带（raw=3F, err=-3.0），
         *   车被判成"弯道"进了 EL_CORNER；而 EL_CORNER 的丢线分支里
         *   **没有盲走判据**（原来只写在 STRAIGHT），于是它绕着残段
         *   以 32 mm/s 爬了 10.5 s，最后触发"丢线超时停车"，整趟作废。
         * 窗口是标定过的"这一段没有弯"，所以跑进来就当断口处理，
         * 不再让灰度的残段把状态机带到别处去。
         *
         * 安全性：blind_go 会置 s_blind_lock → 位置环关掉、航向写死为
         * 发车方向+180°。实测同一套逻辑走 1493 mm 只偏 20 mm。 */
        if (!s_blind_done && blind_win_hit(dist_mm)) {
            /* 线还在时抢进来的：要求先真丢一次线才能退出（见 s_blind_needlost） */
            blind_go(dist_mm, yaw, tick_ms, (uint8_t)(line ? 1u : 0u));
            break;
        }

        /* 0) 【终点】走出盲走之后，第一次"全黑"就是终点停车线。
         *
         * 为什么能这么判：赛道的全黑（8 路全压黑）只有两处 ——
         *   发车/终点那条横跨赛道的黑带，以及路口；
         * 而"丢线"是 0 路亮，跟全黑正好相反，不会混。
         * 盲走之后已经没有直角弯了，唯一还会全黑的就是终点，所以武装置位后
         * 只需要"持续 40 ms"就能确认（比路口的 15 ms 长，避开单帧误判）。
         *
         * 必须放在 junction → EL_CROSS 之前，否则会先被当成路口开过去。 */
        if (s_finish_armed && trk->junction) {
            if (s_t_finish == 0u) s_t_finish = tick_ms;
            if ((tick_ms - s_t_finish) >= CFG_FINISH_MS) {
                s_o.v_cmd   = 0.0f;
                s_o.kp_scale = 0.0f;
                enter(EL_STOP, dist_mm, yaw, tick_ms);
                break;
            }
        } else if (!trk->junction) {
            s_t_finish = 0u;
        }

        /* 1) 全黑：路口 / 停车线（丢线不算，因为丢线时 valid=0）
         *
         * 【2026-11-24 修】加了 `!s_finish_armed`：
         * 原来的写法是"终点判据排在路口之前"，但两条判据用的是**各自的计时器**，
         * 谁先到阈值谁先触发 —— 路口 FSM_JUNCTION_MS=15 ms、终点 CFG_FINISH_MS=40 ms，
         * 所以路口总是先赢，终点永远不生效。
         * 实测 19:36 那趟：车跑到 12362 mm 撞上起跑黑带（raw=00 cnt=8），
         * 进的是 `[el] 9`（路口），不是停车 —— 整趟跑过头又跑了 600 mm 才停。
         * 走出盲走之后赛道上不该再有路口，所以武装后一律按终点处理。 */
        if (trk->junction && !s_finish_armed) {
            if (s_t_junc == 0u) s_t_junc = tick_ms;
            if (tick_ms - s_t_junc >= FSM_JUNCTION_MS) {
                enter(EL_CROSS, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_t_junc = 0u;
        }

        /* 1.5) 【提前入弯】线还没丢、但已经偏到倒数第二路 → 不等丢线就开始转。
         *
         * 为什么必须放在"丢线"分支**之前**：丢线那一拍 bsp_track 已经把 err
         * 清成 0，等进到丢线分支就再也拿不到"偏了多少"这个信息了。
         * 而"偏了多少"正是判断"前方是直角弯还是断口"最直接的量。
         *
         * 为什么能省时间：等丢线再转，车头是正的，必须走完整 90° 圆弧；
         * 线偏到 2.5 时车头已经朝着弯里约 20°，剩下的弧短了，
         * 而且车一直在动（配合 T 命令把 brake 设 0、v 设 130），
         * 省掉"刹停 220 ms + 转完再低速找线"这两段停顿。
         *
         * 触发条件（缺一不可）：
         *   ① P 命令开着（s_pre_on）
         *   ② 已上膛（线居中过，见上面）
         *   ③ 线看得见，且 |err| 连续 HOLD_MS 超过阈值
         *   ④ 离上一次转弯结束超过 COOL_MS
         *   ⑤ 没在判环岛（s_t_edge == 0）：环岛入口的切线看起来也像"线偏到边上"，
         *      而且会持续很久。30 ms 的提前入弯会抢在 150 ms 的环岛判据之前开转，
         *      结果是把环岛入口当成直角弯。这里让环岛判据优先。
         * 方向直接用 err 的符号（正 = 线在右 = 右转），和丢线分支一致。 */
        {
            /* 两路特征，谁先到用谁：
             *   edge —— 线偏到视野边缘（|err| 大）。大弯道（斜着进弯）是这种。
             *   wide —— 线**变宽**（连续压线 >= CFG_PRE_TURN_CNT 路）。
             *           直角弯是这种：车正压在拐点上，来路 + 去路两条胶带同时
             *           压在传感器上。实测 raw=E0 → 压线 5,6,7，err 却只有 +1
             *           —— 只看 err 永远等不到（这也是"纯 PID 判不出来"的原因）。
             *
             * 【2026-11-25 为什么必须加 wide】
             * 实测方场直角弯的时序（run_215809）：
             *   4.189 dist=2450 raw=E0 cnt=5 err=+1   ← 线变宽：拐点在此
             *   4.293 dist=2481 raw=F7 cnt=1          ← 100 ms 后只剩 1 路
             *   4.596 dist=2573 raw=FF cnt=0          ← 彻底丢线
             *   4.700 dist=2603                       ← EL_TURN90 才开始转
             * 也就是说：真正该开转的位置在 2450，实际在 2603 —— **晚了 153 mm**。
             * 而 300 mm/s 下这段要跑 500 ms，用户看到的就是"冲出去好多才转"。
             *
             * 更要紧的是连带后果：车带着 60 mm 的过冲进入旋转，转满 90° 时
             * 新线已经在车身侧向 60 mm 处，超出 ±35 mm 视野 → 看不见 →
             * 继续转到 105° 才让线进入视野边缘（实测 done 总是 104~105）→
             * 转完 cnt=1、再走 100 mm 就丢线。**"冲出去"和"转多了"是同一件事。** */
            /* edge 默认关闭（CFG_PRE_TURN_EDGE_EN=0），原因见 app_config.h：
             * 曲线和环岛都是"单条胶带 + 车偏一侧"，err 很容易到 2.0，
             * 用它触发必然把曲线当直角弯。真直角弯的签名是 cnt>=5。 */
            uint8_t edge = (uint8_t)((CFG_PRE_TURN_EDGE_EN &&
                                      (absf(err) >= s_pre_err)) ? 1u : 0u);
            uint8_t wide = (uint8_t)((trk->count >= CFG_PRE_TURN_CNT &&
                                      trk->count < 8u) ? 1u : 0u);
            /* 环岛区不许抢：六边形入口同样是"线变宽"，而且它要的是"进环岛"
             * 而不是"转 90°"。这里只按"已经走过盲走"来分区 —— 大赛道上
             * 直角弯全在断口之前，走完盲走后没有 90° 弯，所以关掉不吃亏；
             * 正方形闭环永远没走盲走，这条恒不生效。 */
            uint8_t loop_zone = (uint8_t)((CFG_LOOP_AFTER_BLIND &&
                                           (s_blind_done || s_dbg_postblind))
                                          ? 1u : 0u);

            /* 【2026-11-25 加：转完冷却（按里程）】
             * 实测（run_221510，v_turn=40）第 5、6 个弯只隔 **82 mm**：
             * 车转完 81° 时**还站在拐点的宽黑上**（cnt 仍是 5~6），
             * 于是立刻被判成"下一个直角弯"，连着又转 105.9° → 直接出赛道。
             *
             * 时间冷却（CFG_PRE_TURN_COOL_MS=200）挡不住它：整段不到 200 ms。
             * 唯一可靠的是**里程**——转完后必须开出去 CFG_TURN_COOL_MM(150) 才算离开
             * 那个路口。这条和丢线分支用的是同一个冷却量（s_d_turn_exit）。 */
            if (s_pre_on && s_pre_armed && line && (s_t_edge == 0u) &&
                !loop_zone && (edge || wide) &&
                ((uint32_t)(tick_ms - s_t_cool) >= CFG_PRE_TURN_COOL_MS) &&
                ((s_d_turn_exit < 0) ||
                 ((dist_mm - s_d_turn_exit) >= (int32_t)CFG_TURN_COOL_MM))) {
                if (!s_pre_run) {
                    s_pre_run  = 1u;
                    s_t_pre    = tick_ms;
                    s_pre_wide = wide;      /* 记住触发类型，TURN90 里要用 */
                }
                if ((float)(tick_ms - s_t_pre) >= s_pre_hold) {
                    /* C89：声明必须在语句之前 */
                    uint8_t tcnt = (uint8_t)trk->count;
                    int16_t terr = (int16_t)(err * 100.0f);
                    s_pre_run = 0u;
                    enter_turn(EL_TURN90, dist_mm, yaw, tick_ms,
                               turn_dir_corner((int)trk->count,
                                               (int)trk->left_edge,
                                               (int)trk->right_edge, err));
                    s_turn_pre = 1u;        /* enter_turn 会清掉，这里置回 */
                    /* ⚠️ 同一次调用里 s_pre_wide 也被 enter_turn 清了，必须一起置回。
                     * 2026-11-25 实车症状：日志印 pre=1 w=0 c=5 —— 明明是"线变宽"
                     * 触发的，却报成 0，于是 TURN90 里的"中止判据"还开着，
                     * 转过 50° 后线一居中就把真正的直角弯取消掉。 */
                    s_pre_wide    = wide;
                    s_trig_cnt    = tcnt;
                    s_trig_err100 = terr;
                    break;
                }
            } else {
                s_pre_run  = 0u;
                s_pre_wide = 0u;
            }
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
            /* 偏着丢 → 直角弯特征，用快档；居中丢 → 断口/反光，用慢档 */
            if ((tick_ms - s_t_lost) >=
                ((absf(s_err_lv) >= CFG_TURN_ARM_ERR)
                     ? (uint32_t)s_confirm_fast : (uint32_t)s_confirm_slow)) {

                /* ===== 【转弯模式 → 进盲走 · 最高优先级】=====
                 * 任务书：docs/21_blind_trigger_task.md（"三重同意 + 里程兜底"）
                 *
                 * 赛道进盲走区之前是**连续两个右转**，转完几乎马上就到断口。
                 * 判据（缺一不可）：
                 *   ① 已转弯数 >= CFG_BLIND_MIN_TURNS
                 *   ② 最近两次转弯方向都是 CFG_BLIND_REQ_DIR（方向锁打开时改比锁）
                 *   ③ 里程落在 [CFG_BLIND_RUN_MIN, CFG_BLIND_RUN_MAX]（未配置则不查）
                 *   ④ 只触发一次（s_blind_done）
                 * 另外还有一条**里程兜底**：窗口已标定时，只要里程落在窗口内就进盲走，
                 * 不看 ①② —— 这是"漏认一个弯就永远不触发"的保险（任务书 §6.3）。
                 * 注意兜底的存在意味着：窗口一旦配置，①② 就退化为"检查项"而非"必要条件"。
                 *
                 * 为什么不用灰度判断口：断口和直角弯在 8 路上区间重叠，
                 * 前六轮反复验证过不可分（mabs / slope / err_lv 全都试过）。 */
                {
                    /* 判据全部收敛到 blind_want()（三个调用点共用一套，见文件头）。 */
                    if (blind_want(dist_mm)) {
                        blind_go(dist_mm, yaw, tick_ms, 0u);
                        break;
                    }
                }

                /* 【2026-11-25 · 安全闸】绕过环岛之后再丢线，**不许再判直角弯**。
                 * 实测代价（run_223758）：环岛没进去、直着开过去，出来丢线，
                 * 于是被判成直角弯转了 105.6° —— 车直接被扔出赛道，
                 * 接着又误进一次环岛、最后复位，整趟报废。
                 * 赛道上最后一个环岛之后只剩"终点"，不可能再有直角弯。
                 * 所以这里改走 SEEK：低速前进 + 位置环，找得到线就继续，
                 * 找不到就由 CFG_LOST_STOP_MS 兜底停车。**宁可慢，不许转错。** */
                if (s_loop_done) {
                    enter(EL_SEEK, dist_mm, yaw, tick_ms);
                    break;
                }

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

                /* 【转完冷却 2026-11-24】刚转完不到 CFG_TURN_COOL_MM 就又丢线：
                 * 这不是"前方又一个直角弯"，而是转完还没稳住的残余误差
                 * （实测 ±3.0，出现在转完 50~150 mm 处）。在这里再转一次，
                 * 累计就超过 200° —— 实测表现为整车掉头，之后所有弯方向全反。
                 * 改成低速 SEEK：慢速前进 + 位置环，把线找回来。 */
                if ((CFG_TURN_COOL_MM > 0u) &&
                    ((dist_mm - s_d_turn_exit) < (int32_t)CFG_TURN_COOL_MM)) {
                    enter(EL_SEEK, dist_mm, yaw, tick_ms);
                    break;
                }

                enter_turn(EL_TURN90, dist_mm, yaw, tick_ms, turn_dir_from_err());
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
        /* 【2026-11-24 加闸】只在"已经走过盲走"之后才判环岛。
         * 原因：直角弯在丢线前那一两拍，胶带会横扫过多路（实测能到 5~6 路连续），
         * 形态和六边形分叉一模一样。一旦在弯里误进 EL_LOOP_IN，入口段会强制加
         * 偏置 2.5（见下面的实现），车就被从弯里拽出去 —— 现场表现就是
         * "在转弯那里脱线、然后直着冲出去"。
         * 本图的六边形在断口之后，加这个门不影响它；右上那个圆形环岛在断口之前，
         * 会被这道闸挡掉（靠普通循迹过，和之前行为一致）。
         * 换到"环岛在断口之前"的赛道时，把 CFG_LOOP_AFTER_BLIND 置 0 即可。 */
        /* 【2026-11-24 放宽】只看"连续压线 ≥5 路"，不再要求"贴到某一侧边缘"。
         * 实测同一个六边形顶点，两次跑的形态不一样（取决于车的横向位置）：
         *   · mask=FC（压线 2~7，右侧到边）—— 满足旧的"贴边"条件
         *   · mask=7C（压线 2~6，两侧都不沾边）—— 旧条件直接判否，环岛就不触发
         * 正常循迹时连续压线只有 2~3 路，≥5 路只可能是"两条线并到一起"，
         * 所以条数本身就是足够强的特征；贴边与否只是视角差异，不该当判据。
         * 防止直角弯误判靠的是上面那道 s_blind_done 闸，不是这里。 */
        if (line && (CFG_LOOP_AFTER_BLIND == 0 || s_blind_done || s_dbg_postblind) &&
            ((s_d_loop_exit < 0) ||
             ((dist_mm - s_d_loop_exit) >= (int32_t)CFG_LOOP_COOL_MM)) &&
            trk->count >= FSM_LOOP_SIDE_CNT) {
            if (s_t_edge == 0u) s_t_edge = tick_ms;
            s_t_edge_last = tick_ms;       /* 记下这次命中 */
            if ((uint32_t)(tick_ms - s_t_edge) >= CFG_LOOP_ARM_MS) {
                s_d_loop  = dist_mm;
                s_yaw_acc = 0.0f;              /* 从这里开始累计转角 */
                /* 方向：优先用配置（本图六边形在右侧），其次看哪一侧贴到边。
                 * 不看 err 符号 —— 顶点处 err≈0，符号是随机的。 */
                if (CFG_LOOP_EDGE_DIR != 0) {
                    s_loop_dir = (int8_t)CFG_LOOP_EDGE_DIR;
                } else if (trk->right_edge >= 6) {
                    s_loop_dir = 1;
                } else if (trk->left_edge <= 1) {
                    s_loop_dir = -1;
                } else {
                    s_loop_dir = 1;      /* 对称宽黑时兜底按右侧 */
                }
                /* 本图的六边形要绕满 360°，圆形环岛同理，统一按 360° 表处理 */
                s_loop_kind = LOOP_HEX;
                s_loop_yaw0 = yaw;      /* 强制转弯的基准航向 */
                s_o.e_bias = (s_loop_dir < 0) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS;
                enter(EL_LOOP_IN, dist_mm, yaw, tick_ms);
                break;
            }
        } else if ((s_t_edge != 0u) &&
                   (uint32_t)(tick_ms - s_t_edge_last) <= CFG_LOOP_GAP_MS) {
            /* 【2026-11-24 修】宽黑形态允许**间断**，不清零。
             * 实测六边形入口（本赛道盲走出口后约 1.2 m）的形态是：
             *   cnt=6（mask=FC，右侧到边）→ 100 ms 后 cnt=4（mask=3C，两侧都不沾边）
             *   → 再 100 ms 又回到 cnt=6（mask=7E）
             * 判据本身每一帧都满足，但要求"连续 150 ms"就永远凑不够 ——
             * 和 EL_SEEK 当年"看见就算"是同一类毛病：把间断当成了失败。
             * 现在只要两次命中之间不超过 CFG_LOOP_GAP_MS(200) 就继续累计。
             *
             * 但**丢线必须清零**：直角弯的特征是"贴边之后整片消失"，
             * 如果丢线还宽容，拐弯会被误判成环岛（这正是 2026-11-22 加 150 ms 的原因）。
             * 所以这里只在 `line` 仍为真时宽容。 */
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

        /* 【写死航向的盲走】数够 N 个直角弯之后那次。
         * 地上是一片空白，残留假线只会把方向盘带歪，所以位置环直接关掉，
         * 目标航向用绝对角度（发车方向 + 180），不用"进弯时的当前朝向" ——
         * 这样前 7 个弯每次差 1° 的累积误差不会被带进这 1.5 m。
         * 值的写入在 ctrl_core 的元素切换处（只在切换那一刻生效一次）。 */
        if (s_blind_lock) {
            s_o.kp_scale   = 0.0f;
            s_o.yaw_hard   = 1u;
            s_o.blind_lock = 1u;
            s_o.yaw_target = (CFG_BLIND_USE_ABS_YAW)
                             ? wrap180f(s_start_yaw + CFG_BLIND_YAW_OFFSET)
                             : s_blind_yaw0;
        }

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

        /* 真丢过一次线才允许退出（抢跑进来的那次） */
        if (!line) s_blind_needlost = 0u;

        if (line && gap_ready && !s_blind_needlost) {
            /* 【环岛入口快判】盲走区后面直接接环岛时，线一出现就"一侧全亮"。
             * 不在这里接住的话，车会先沿盲走方向直冲 45~90 mm，
             * 等 EL_STRAIGHT 攒够 150 ms 才确认，很可能已错过入口切线。
             *
             * ⚠️ 触发环岛必须"三重同意"，缺一不可：
             *   ① 转弯次数：s_blind_lock == 1。
             *      这个标志只有"数够 N 个弯 且 最后两次方向一致"那次盲走才会有；
             *      里程窗、Z 强制盲走、误判进来的盲走都不带它。
             *      —— 光看线形状是不够的，前半程任何一处贴边都会被误认成环岛。
             *   ② 里程：已经跑到 CFG_LOOP_RUN_MIN_MM 之后（相对发车原点）。
             *      实测六边形环岛入口在 dist≈9442，给 8000 当下限。
             *   ③ 形状：一侧贴边 + ≥3 路，或双侧贴边（raw=E3 这种主线+切线分叉）。
             *
             * 三重都满足才进 EL_LOOP_IN；任一条不满足都回 EL_STRAIGHT，行为与原来一致。
             *
             * 偏置方向必须由"贴边侧"决定，不能用 err 符号：线刚出现那一拍
             * err 可能接近 0，符号随机；实测偏反过一次，入口 82 mm 内右甩 23°。 */
            float   bias_sgn  = 0.0f;
            uint8_t shape_ready;
            int32_t run_mm    = dist_mm - s_d_run0;

            /* 记下"第一次看到线"的里程：形状还没辨清时，用它做出口延迟的基准 */
            if (s_d_firstline < 0) s_d_firstline = dist_mm;

            /* ① ② 先过"次数 + 里程"这两关，才轮到看形状 */
            if ((CFG_BLIND_NEXT_LOOP != 0) && (s_blind_lock != 0u) &&
                (run_mm >= CFG_LOOP_RUN_MIN_MM) && trk) {
                uint8_t l_e = (uint8_t)((trk->left_edge  == 0) ? 1u : 0u);
                uint8_t r_e = (uint8_t)((trk->right_edge == 7) ? 1u : 0u);

                if ((trk->count >= 3u) && (l_e || r_e)) {
                    s_loop_seen = 1u;
                    if (l_e && !r_e) {
                        bias_sgn = -1.0f;      /* 只有左侧贴边 → 环岛在左 */
                    } else if (r_e && !l_e) {
                        bias_sgn =  1.0f;      /* 只有右侧贴边 → 环岛在右 */
                    } else {
                        bias_sgn = (err < 0.0f) ? -1.0f : 1.0f;   /* 两侧都贴，退回符号 */
                    }
                    s_loop_bias = bias_sgn;    /* 锁存，供出口那一拍使用 */
                }
#if (CFG_BLIND_LOOP_FORCE == 2)
                /* 最宽松档：>=2 路就进，正常不该用到，留作兜底 */
                if (!s_loop_seen && (trk->count >= 2u)) {
                    s_loop_seen = 1u;
                    bias_sgn    = (err < 0.0f) ? -1.0f : 1.0f;
                    s_loop_bias = bias_sgn;
                }
#endif
            }

            /* 形状还没辨清就先别退（见文件头 s_loop_seen 注释）。
             * 已辨清 → 2 拍就走；一直辨不清 → 最多再走 CFG_BLIND_EXIT_MM。 */
            shape_ready = (uint8_t)((s_loop_seen != 0u) ||
                                    ((s_d_firstline >= 0) &&
                                     ((dist_mm - s_d_firstline) >= (int32_t)CFG_BLIND_EXIT_MM)));

            if ((++s_relock >= FSM_RELOCK_CYCLES) && shape_ready) {
                /* 出盲走 = 这圈已经跑过大半，之后第一次全黑就是终点停车线 */
                s_finish_armed = 1u;
                if (s_loop_seen) {
                    s_loop_kind = (CFG_BLIND_NEXT_LOOP == 2) ? LOOP_CIRCLE : LOOP_HEX;
                    s_yaw_acc   = 0.0f;          /* 从这里开始累计绕圈角度 */
                    s_d_loop    = dist_mm;
                    s_o.e_bias  = s_loop_bias * FSM_LOOP_BIAS;
                    enter(EL_LOOP_IN, dist_mm, yaw, tick_ms);
                } else {
                    enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                }
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
        /* 【2026-11-23 改】入口段不再无条件偏置。
         * 原来无论看不看得见线都加 ±FSM_LOOP_BIAS(2.5)，80 mm 内能把车甩 20 多度：
         * 实测 yaw -9.6 → -32.6，把刚抓到的线从右侧甩到了左侧（err +0.5 → -3.0）。
         * 入口处线通常是看得见的（环岛切线），有线就跟线；
         * 只有线真的丢了（六边形顶点的尖角）才用偏置兜住。 */
        /* 【2026-11-24 改回"一直带偏置"】入口段必须主动选边。
         * 2026-11-23 曾改成"有线就不偏"，是为了救圆形环岛（偏置 2.5 会甩过头 20°+）。
         * 但本图六边形的顶点是**6 路对称宽黑**（实测 raw=C0/E0/C1，err≈0）：
         * 位置环看到的误差就是 0，没有任何理由往环岛那边打 —— 车必然直着过去。
         * 偏置 2.5 在 80 mm 内能把航向拉 20~30°，正好够咬住六边形那条边。
         * 圆形环岛如果因此甩过头，把 CFG_LOOP_ENTRY_MM 调小即可（80 → 50）。 */
        if (CFG_LOOP_ENTRY_DEG > 0.5f) {
            /* 【2026-11-25】强制转 CFG_LOOP_ENTRY_DEG 度，不跟线。
             * 原因见 app_config.h：顶点处 err≈0，偏置只会平移不会转弯。
             * 转到位（或走够里程上限）就交给 LOOP_RUN 跟线。 */
            float dh;
            s_o.e_bias     = 0.0f;
            s_o.yaw_hold   = 0u;
            s_o.kp_scale   = 0.0f;
            s_o.w_mode     = 1u;
            s_o.yaw_target = wrap180f(s_loop_yaw0 +
                                      g_yaw_sgn * (float)s_loop_dir *
                                      CFG_LOOP_ENTRY_DEG);
            set_speed_target(s_v_loop[s_loop_kind]);
            dh = wrap180f(s_o.yaw_target - yaw);
            if ((absf(dh) < CFG_LOOP_ENTRY_TOL) ||
                ((dist_mm - s_d_loop) > (int32_t)CFG_LOOP_ENTRY_MM)) {
                s_o.w_mode = 0u;
                enter(EL_LOOP_RUN, dist_mm, yaw, tick_ms);
            }
            break;
        }

        /* 旧行为（CFG_LOOP_ENTRY_DEG=0）：横向偏置 */
        s_o.e_bias   = (s_loop_dir < 0) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS;
        s_o.yaw_hold = 0u;
        s_o.kp_scale = 1.0f;
        s_o.w_mode   = 0u;
        set_speed_target(s_v_loop[s_loop_kind]);
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
                            : ((s_loop_dir < 0) ? -FSM_LOOP_BIAS : FSM_LOOP_BIAS);
        set_speed_target(s_v_loop[s_loop_kind]);

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
        /* 【2026-11-24】出环岛 = 这圈已经跑完大半，之后第一次全黑就是终点停车线。
         * 原来 s_finish_armed 只在"走出盲走"时置位，于是：
         *   · 调试模式（N 1，从断口起步、没走盲走）——终点黑带被当路口开过去；
         *   · 万一哪天盲走被跳过，整圈也不会停。
         * 放在这里最自然：环岛之后赛道上只剩"终点"一个黑带。 */
        s_finish_armed = 1u;

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
            if ((tick_ms - s_t_lost) >=
                ((absf(s_err_lv) >= CFG_TURN_ARM_ERR)
                     ? (uint32_t)s_confirm_fast : (uint32_t)s_confirm_slow)) {
                /* 【断口优先 · 2026-11-24】盲走判据在 CORNER 里同样要生效。
                 * 原来只写在 EL_STRAIGHT 的丢线分支里，车一旦先被判进 CORNER
                 * （断口前的胶带残段会让 |err| 冲到 3.0，50 ms 就够进 CORNER），
                 * 再丢线时只会走下面的"直角弯"分支 —— 盲走**永远不触发**。
                 * 实测代价：绕着残段以 32 mm/s 爬 10.5 s，最后丢线超时停车。 */
                if (blind_want(dist_mm)) {
                    blind_go(dist_mm, yaw, tick_ms, 0u);
                    break;
                }

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
                    enter_turn(EL_TURN90, dist_mm, yaw, tick_ms, turn_dir_from_err());
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
     * 直接给角速度目标。前进速度用 g_turn_v（默认 0 = 两轮 ±dv 对称反转，
     * 纯原地转，车心不动），这样转完 90° 车还在路口，新线仍在视野边缘，
     * 不需要靠"多转 15° 斜插"去补位（见 app_config.h 里 CFG_V_TURN 的说明）。
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
         * 加一个固定的刹车时间，让每次转弯都从 v≈0 开始，行为就一致了。
         *
         * 【提前入弯不刹车】提前入弯的前提就是"不停车"：它是线还没丢的时候
         * 主动开转的，如果还被这 220 ms 刹停挡一下，等于"提前停车再原地转"，
         * 比原方案更慢。所以 s_turn_pre 时直接跳过刹车相。
         * 要恢复"先停再转"，发 P 0 关掉提前入弯即可。 */
        {
            /* 提前入弯用独立、更短的刹车（CFG_PRE_TURN_BRAKE_MS）。
             * 完全不刹（原来的做法）在 300 mm/s 下进弯，轮速要跳变 ~700 mm/s，
             * 电机跟不上，前 200 ms 是滑行，每次滑多远都不一样 —— 实测转角
             * 离散从 8° 涨到 37° 就是这么来的。给 50 ms 只花 15 mm，
             * 但把每次入弯的初速拉到同一量级。 */
            uint32_t brake_ms = s_turn_pre ? (uint32_t)CFG_PRE_TURN_BRAKE_MS
                                           : (uint32_t)g_turn_brake;
            if ((uint32_t)(tick_ms - s_t_enter) < brake_ms) {
                s_o.w_mode   = 0u;      /* 不转 */
                s_o.kp_scale = 0.0f;    /* 也不用位置环 */
                set_speed_target(0.0f); /* 刹停 */
                break;
            }
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

        /* 【提前入弯 · 中止】转过 ABORT_DEG 之后线还在、而且已经基本居中，
         * 说明这**不是** 90° 直角弯（可能是左上那段 45° 斜切，或者一开始就判早了）。
         *
         * 这一步是原方案漏掉的、但必须有的：EL_TURN90 里 kp_scale=0，
         * 8 路位置环是**关**的，车不会"被循迹拉回来"，只会硬转满 g_turn_deg。
         * 45° 的弯被转成 90°，车就斜着冲出赛道了。
         *
         * 要求连续 2 拍成立、且"压线路数 ≥2"：旋转过程中传感器会扫过原来那条线，
         * 单拍、单路的瞬时居中不能当中止依据（那只是擦过，不是对正）。 */
        if (s_turn_pre && !s_pre_wide && line && (trk->count >= 2u) &&
            (absf(err) < CFG_PRE_TURN_ABORT_ERR) &&
            (absf(wrap180f(yaw - s_turn_start)) >= CFG_PRE_TURN_ABORT_DEG)) {
            if (++s_pre_ok >= FSM_RELOCK_CYCLES) {
                s_o.w_mode = 0u;
                s_turn_pre = 0u;
                s_pre_ok   = 0u;
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_pre_ok = 0u;
        }

        /* 【开环模式 · CFG_TURN_MODE=1】固定差速 + 定时，完全不碰陀螺仪。
         *
         * 为什么留这条路：闭环在打滑严重时是在追一个追不到的目标
         * （指令 220°/s，实际只跑出 42°/s），P 项长期饱和，每次停在哪
         * 取决于当次打滑，反而不可预测。开环给固定差速时实际角速度是稳的，
         * "转多少度 = 角速度 × 时间"，标定一次就够。
         *
         * 计时从刹车相结束算起，所以要把 g_turn_brake 扣掉。
         * 转完直接进 SEEK 找线，不再做角度到位判定。 */
        if (g_turn_mode > 0.5f) {
            s_o.kp_scale   = 0.0f;
            s_o.e_bias     = 0.0f;
            s_o.yaw_hold   = 0u;
            s_o.w_mode     = 2u;                                  /* 开环差速 */
            s_o.dv_open    = s_turn_dir * g_turn_open_dv;         /* 已带方向 */
            set_speed_target(0.0f);                               /* 两轮 ±dv，纯原地转 */

            if ((float)(tick_ms - s_t_enter - (uint32_t)g_turn_brake) >= g_turn_open_ms) {
                s_o.w_mode = 0u;
                enter(EL_SEEK, dist_mm, yaw, tick_ms);
            }
            break;
        }

        s_o.kp_scale   = 0.0f;
        s_o.e_bias     = 0.0f;
        s_o.yaw_hold   = 0u;
        s_o.w_mode     = 1u;
        s_o.yaw_target = s_scan_back ? s_turn_start : s_turn_target;
        set_speed_target(g_turn_v);

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
                } else if ((s_turn_pre || line) && line &&
                           (absf(err) < CFG_TURN_FIND_ERR)) {
                    /* 【2026-11-24 连贯性】转弯是因为"线已经居中"才收的手
                     * （|err| < CFG_TURN_FIND_ERR=1.0），那就说明已经对上，
                     * 没必要再进 SEEK 低速蹭一遍 —— 直接回直道全速。
                     * 实测每弯能省 150~250 ms，7 个弯约 1.4 s，而且车不停顿、更连贯。
                     * 只有"转满 CFG_TURN_DEG 还看不到居中"的情况才走 SEEK。 */
                    enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
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
         *
         * 【2026-11-24 修：必须再有上限，这是"掉头 180°"的直接原因】
         * 原来只有下限（转过 ≥95°），没有上限。实测：车打滑冲过目标后还在转，
         * 转到 ~180° 时**来时的旧线正好居中**（err<1.0），于是被判成"找到新线"
         * 就地停住 —— 整车方向掉头 180°，之后所有弯的方向全部反号。
         * 所以"线居中"只在转角 ∈ [EARLY_MIN_DEG, deg + MAX_EXTRA] 之内才算数；
         * 超过就直接收手（去 SEEK 低速找线），绝不允许继续转到旧线上。 */
        {
            float rot = absf(wrap180f(yaw - s_turn_start));
            /* 下限（防"扫过旧线"被当成对正）：
             * 从**丢线**起转的，车头已经过冲，必须转满 95° 才可能看到新线；
             * 从**拐点**起转的（提前入弯），90° 就到位，95° 会白转 5°，
             * 所以用更低的下限。 */
            float early_min = s_turn_pre ? CFG_TURN_EARLY_MIN_DEG_PRE
                                         : CFG_TURN_EARLY_MIN_DEG;

            if (line && absf(err) < CFG_TURN_FIND_ERR &&
                rot >= early_min &&
                rot <= (g_turn_deg + CFG_TURN_MAX_EXTRA_DEG)) {
                if (++s_relock >= FSM_RELOCK_CYCLES) {
                    s_o.w_mode = 0u;
                    /* 提前入弯本来就在动，线也看得见 → 直接回 STRAIGHT */
                    if (s_turn_pre) enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                    else            enter(EL_SEEK, dist_mm, yaw, tick_ms);
                    break;
                }
            } else {
                s_relock = 0u;
            }

            /* 转过头上限：还没"线居中"也必须收手。
             * s_scan_back 时不能截断 —— 那是"确认是断口、正往回转"的路径，
             * 转角本来就要越过 deg 才能回到原朝向。 */
            if (!s_scan_back && (rot > (g_turn_deg + CFG_TURN_MAX_EXTRA_DEG))) {
                s_o.w_mode = 0u;
                enter(EL_SEEK, dist_mm, yaw, tick_ms);
                break;
            }
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

        /* 【回中判据 2026-11-24】必须把 err 拉回 CFG_SEEK_CENTER_ERR 以内才算
         * "重新锁定"，才允许回 STRAIGHT。
         *
         * 原来这里只有 `if (line)`：刚转完线还在视野最外沿（err=±3.0、cnt=1）
         * 也算"找到"，于是 50 ms 内就跳回 STRAIGHT，用 300 mm/s 全速去追 30 mm
         * 偏差（w_ref=55*3.0=165°/s）→ 一把扫过线冲到另一侧 → 再拉回来 → 一路摆。
         * 实测日志里 [el] 13 常常根本不出现，就是 SEEK 只活了不到一拍。
         *
         * 现在：低速（180）+ 位置环打开，先在 SEEK 里把车拉回中间再走。 */
        if (line && (absf(err) < CFG_SEEK_CENTER_ERR)) {
            if (++s_relock >= FSM_RELOCK_CYCLES) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
        } else {
            s_relock = 0u;
        }

        /* 超时：回中的过程比"看到线"慢得多，所以超时也放长了（1200 ms）。
         * 而且**只要还看得见线就不停车** —— 位置环是开的，交回 STRAIGHT 只是
         * "带着偏移继续跟线"，不会跑飞；停车的坏处远大于好处。 */
        if (dwell > CFG_SEEK_TIMEOUT_MS) {
            if (line) {
                enter(EL_STRAIGHT, dist_mm, yaw, tick_ms);
                break;
            }
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

/* 环岛速度（串口 S 命令）：v <= 0 表示不改 */
void fsm_set_loop_speed(float v_circle, float v_hex)
{
    if (v_circle > 0.0f) s_v_loop[LOOP_CIRCLE] = v_circle;
    if (v_hex    > 0.0f) s_v_loop[LOOP_HEX]    = v_hex;
}

void fsm_get_loop_speed(float *v_circle, float *v_hex)
{
    if (v_circle) *v_circle = s_v_loop[LOOP_CIRCLE];
    if (v_hex)    *v_hex    = s_v_loop[LOOP_HEX];
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

/* ---------------- 提前入弯（串口 'P' 命令） ----------------
 * on 传 <0 表示不改。err 传 <=0 表示不改（阈值不可能是负的）。
 * 为什么做成在线可调：它是"要不要提前开转"的开关，
 * 现场两种跑法（P 0 / P 1）各跑一圈，比任何离车推算都有说服力。 */
void fsm_set_preturn(int on, float err, float hold_ms)
{
    if (on >= 0)          s_pre_on   = (on != 0) ? 1u : 0u;
    if (err > 0.0f)       s_pre_err  = err;
    if (hold_ms > 0.0f)   s_pre_hold = hold_ms;
    /* 改参数时把计时和上膛状态清掉：否则旧阈值攒下的 30 ms
     * 加上新阈值，会在"刚改完参数"的那一拍立刻触发一次。 */
    s_pre_run   = 0u;
    s_pre_armed = 0u;
}

void fsm_get_preturn(int *on, float *err, float *hold_ms)
{
    if (on)      *on      = (int)s_pre_on;
    if (err)     *err     = s_pre_err;
    if (hold_ms) *hold_ms = s_pre_hold;
}

/* 1 = 当前这次 TURN90 是提前入弯触发的（日志区分用） */
uint8_t fsm_turn_preturn(void) { return s_turn_pre; }

/* 1 = 这次提前入弯由"线变宽（cnt>=CFG_PRE_TURN_CNT）"触发，
 * 0 = 由"线偏到边缘（|err| 大）"触发或不是提前入弯。日志区分用。 */
uint8_t fsm_turn_wide(void) { return s_pre_wide; }

/* 触发瞬间的形态快照：cnt = 连续压线路数，err100 = err×100。
 * 用来在日志里一眼分辨"真直角(cnt>=5)"和"曲线(cnt<=3)"。 */
void fsm_turn_trig(int *cnt, int *err100)
{
    if (cnt)    *cnt    = (int)s_trig_cnt;
    if (err100) *err100 = (int)s_trig_err100;
}

/* ---------------- 转弯方向锁（串口 'D' 命令） ----------------
 * dir: +1 强制右转 / -1 强制左转 / 0 恢复"看 err 符号"。
 * 符号约定与 g_yaw_sgn 无关：+1 永远是物理右转。 */
void fsm_set_dir(int dir)
{
    if (dir > 0)      s_dir_force =  1;
    else if (dir < 0) s_dir_force = -1;
    else              s_dir_force =  0;
}

int fsm_get_dir(void) { return (int)s_dir_force; }

/* ---------------- 调试：假装盲走已走完（串口 K 命令） ----------------
 * 1 = 允许判环岛入口 / 0 = 关闭。掉电与发车都不会自动清，只有 K 0 才清。 */
/* 丢线确认时间（串口 R）：0 = 不改 */
void fsm_set_confirm_ms(int fast_ms, int slow_ms)
{
    if (fast_ms > 0) s_confirm_fast = (uint16_t)fast_ms;
    if (slow_ms > 0) s_confirm_slow = (uint16_t)slow_ms;
}

void fsm_get_confirm_ms(int *fast_ms, int *slow_ms)
{
    if (fast_ms) *fast_ms = (int)s_confirm_fast;
    if (slow_ms) *slow_ms = (int)s_confirm_slow;
}

void fsm_set_postblind(int on) { s_dbg_postblind = (uint8_t)(on ? 1 : 0); }
int  fsm_get_postblind(void)   { return (int)s_dbg_postblind; }

/* ---------------- 转弯计数（串口 'C' 命令） ----------------
 * 赛道一圈 7 个直角弯，转够之后下一次丢线就是断口盲走区，
 * 所以"C 命令"既是查看进度，也是标定"在哪个弯之后进盲走"。
 *
 * reset != 0 → 计数清零（新的一圈）
 * after >  0 → 设置"转够这么多个弯后进盲走"
 * after <= 0 → 不动阈值 */
void fsm_set_turncount(int min_turns, int req_dir, int reset)
{
    if (reset) {
        s_turn_cnt   = 0u;
        s_blind_done = 0u;
        s_dir_h1     = 0;
        s_dir_h2     = 0;
    }
    if (min_turns > 0)                 s_blind_min_cnt = (float)min_turns;
    if (req_dir >= -1 && req_dir <= 1) s_blind_req_dir = (int8_t)req_dir;
}

/* ---------------- 盲走里程窗（串口 'M' 命令） ----------------
 * min/max 传 0 表示"这一项不改"；两个都为 0（或 max <= min）表示关闭检查。 */
void fsm_set_blind_win(int32_t min_mm, int32_t max_mm)
{
    if (min_mm >= 0) s_blind_win_min = min_mm;
    if (max_mm >= 0) s_blind_win_max = max_mm;
}

void fsm_get_blind_win(int32_t *min_mm, int32_t *max_mm)
{
    if (min_mm) *min_mm = s_blind_win_min;
    if (max_mm) *max_mm = s_blind_win_max;
}

void fsm_get_turncount(int *cnt, int *min_turns, int *req_dir, int *used,
                       int *h1, int *h2)
{
    if (cnt)       *cnt       = (int)s_turn_cnt;
    if (min_turns) *min_turns = (int)(s_blind_min_cnt + 0.5f);
    if (req_dir)   *req_dir   = (int)s_blind_req_dir;
    if (used)      *used      = (int)s_blind_done;
    if (h1)        *h1        = (int)s_dir_h1;
    if (h2)        *h2        = (int)s_dir_h2;
}

/* 发车航向（盲走绝对目标的基准），给日志用 */
float fsm_start_yaw(void) { return s_start_yaw; }

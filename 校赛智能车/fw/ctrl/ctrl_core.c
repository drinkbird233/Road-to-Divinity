/**
 * @file  ctrl_core.c
 *
 * 符号约定（全工程统一，改之前先想清楚）：
 *   e > 0       线在车右边
 *   w > 0       车头向右偏（顺时针）
 *   dv > 0      左轮快、右轮慢 → 右转
 *   HWT101 yaw  逆时针为正，所以 gyro_right = -imu.gyro_dps
 *
 * 串级结构：
 *   位置环:  w_ref = Kp*(e + e_bias) + 视觉前馈 + 航向保持
 *   角速度环: dv = RATE_FF*w_ref + rate_kp*(w_ref - gyro_right)
 *   速度环:  duty = FF*v_ref + PI(v_ref - v_meas)
 */
#include "ctrl_core.h"
#include "app_config.h"
#include "ctrl_fsm.h"
#include "bsp_motor.h"
#include "bsp_encoder.h"
#include "bsp_track.h"
#include "bsp_imu.h"
#include "bsp_vision.h"
#include "bsp_ui.h"
#include "pid.h"
#include "main.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static car_state_shadow_t s_sh;
static car_params_t       s_p;

/* ---- 定角转弯的在线参数（T 命令改；默认值来自 app_config.h） ----
 * 放在这里而不是宏里，是为了现场扫参数不用反复编译：
 * 每改一次宏要重编 + 烧录约 2 分钟，而一次实车只能试 3~5 个弯。 */
float g_turn_kp  = CFG_TURN_KP;
float g_turn_acc = CFG_TURN_ACC;   /* 刹车曲线的等效角加速度 [°/s²] */
float g_turn_deg = CFG_TURN_DEG;   /* 目标转角 [°]，ctrl_fsm 进弯时读 */
float g_turn_v   = CFG_V_TURN;     /* 转弯前进速度 [mm/s]，0 = 纯原地差速转 */
float g_turn_brake = (float)CFG_TURN_BRAKE_MS; /* 进弯刹车相时长 [ms] */
float g_turn_inner = CFG_TURN_INNER; /* 内轮差速倍率，1.0 = 对称，>1 = 内轮多转 */

static pid_t   s_pid_l;
static pid_t   s_pid_r;

/* 编码器测速滑动窗口。
 * 5 ms 只有 ~10 个脉冲（300 mm/s），1 个计数 = 31 mm/s，
 * 这么粗的量化被 P 放大后必然形成极限环。
 * 把最近 N 拍的计数累加再算速度，分辨率提高 N 倍，代价是 N 拍延迟。 */
#define SPEED_WIN_N 4u
typedef struct {
    int16_t buf[SPEED_WIN_N];
    uint8_t idx;
    uint8_t fill;
    int32_t sum;
} speed_win_t;

static speed_win_t s_win_l;
static speed_win_t s_win_r;

static float speed_window(speed_win_t *w, int16_t d, float fs)
{
    if (w->fill >= SPEED_WIN_N) {
        w->sum -= w->buf[w->idx];
    } else {
        w->fill++;
    }
    w->buf[w->idx] = d;
    w->sum += d;
    w->idx = (uint8_t)((w->idx + 1u) % SPEED_WIN_N);
    return (float)w->sum * CFG_MM_PER_COUNT * fs / (float)w->fill;
}

static lpf1_t  s_lpf_vl;
static lpf1_t  s_lpf_vr;
static lpf1_t  s_lpf_e;
static lpf1_t  s_lpf_gyro;

/* 循迹误差的上次值，用于 PD 的微分项 */
static float   s_e_prev;
/* PD 的微分值，只在循迹更新拍刷新（100 Hz），空拍沿用 */
static float   s_de;
/* 最后一次"看到线"的误差和时刻（补线的起点） */
static float   s_e_last;
static uint32_t s_e_last_ms;
/* 补线：误差/里程历史，用来算"误差随里程的斜率" */
static float   s_ph_e[CFG_PRED_HIST];
static int32_t s_ph_d[CFG_PRED_HIST];
static uint8_t s_ph_n;
static float   s_pred_slope;      /* err per mm */
static int32_t s_e_last_d0;       /* 最后一次看到线时的里程 */

static track_feat_t s_feat;
static imu_t        s_imu;
static vision_t     s_vis;

static float    s_v_meas_l;static float    s_v_meas_r;
static float    s_yaw_ref;
static int32_t  s_dist_mm;
/* 【2026-11-22 修】里程用浮点累加，对外仍以整数 mm 暴露。
 *
 * 原来写的是 s_dist_mm += (int32_t)(v * 0.005)，每拍增量小于 1 mm 就被截成 0：
 *   v=100 mm/s → 0.50 mm → 截成 0（完全不计）
 *   v=150 mm/s → 0.75 mm → 截成 0（完全不计）
 *   v=300 mm/s → 1.50 mm → 截成 1（少记 33%）
 * 结果是 dist 只在 v>=200 时才增长，且比例随车速变化，不能用系数修正。
 * 手推车测里程（约 100~150 mm/s）会得到恒为 0 的 dist。
 * 改成浮点累加后，任何速度都按真实值累计。 */
static float    s_dist_acc;
static int16_t  s_remote_l;
static int16_t  s_remote_r;

static uint8_t  s_mode;
static uint8_t  s_estop;
static uint8_t  s_vis_gap;      /* 视觉预告：前方是断口（最近一帧且在超时内） */
static uint16_t s_fault;
static uint16_t s_fsm_div;
static uint32_t s_tick_ms;

/* 台架模式：绕过整个控制环，直接给占空比。只用于上电验证电机/编码器方向 */
static uint8_t  s_bench;
static float    s_bench_l;
static float    s_bench_r;
static uint8_t  s_hold;          /* 速度保持模式（双轮闭环） */
static float    s_hold_l;
static float    s_hold_r;
static int8_t   s_track_rc;      /* 上次循迹 I2C 读取返回码，供台架诊断 */
static uint8_t  s_trk_div;       /* 循迹读取降频计数器（100 Hz） */
static uint32_t s_ticks;         /* 控制环节拍计数，用于实测循环频率 */
static uint32_t s_track_ok_ms;
static uint32_t s_cmd_ms;
static uint32_t s_vbat_ms;
static uint16_t s_vbat_mv;

/* 上一拍的元素编号，用于检测"元素切换"这个瞬间 */
static uint8_t  s_el_prev = 0xFFu;

/* ---- 定角转弯（台架 u 命令，也是 EL_TURN90 的验证手段） ---- */
static uint8_t  s_turn;
static float    s_turn_target;
static uint32_t s_turn_ms;
static uint32_t s_turn_ok_ms;

static float wrap180f(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

/* 定角转弯的角速度指令（台架 u 命令与 EL_TURN90 共用这一份，保证"台架调出来的
 * 数上车就是同一个行为"）。
 *
 *   w = sgn * max( KP*|dh| , sqrt(2*A*|dh|) )，截到 ±W_MAX
 *
 * 为什么需要 sqrt 这一项：纯 P 在尾段把指令压到 10 °/s 上下，差速 13 mm/s
 * 已经低于电机死区（60），轮子推不动，实测最后 10° 要磨 1.2 s。
 * sqrt 是"以恒定角加速度 A 刹向目标"的速度曲线，越接近目标降得越缓，
 * 既能把尾段顶出死区，又天然不会在目标前还保持高速（不需要微分项）。
 *
 * dh 为 0 时两项都是 0，所以不会自激。 */
static float turn_rate_cmd(float dh, float sgn)
{
    float adh = fabsf(dh);
    float wp  = g_turn_kp * adh;
    float wbr = sqrtf(2.0f * g_turn_acc * adh);
    float wm  = (wbr > wp) ? wbr : wp;

    if (wm >  CFG_W_MAX) wm =  CFG_W_MAX;
    if (wm < -CFG_W_MAX) wm = -CFG_W_MAX;

    return (dh >= 0.0f) ? (sgn * wm) : (-sgn * wm);
}

car_state_shadow_t *ctrl_shadow(void) { return &s_sh; }
car_params_t       *ctrl_params(void) { return &s_p; }
uint8_t ctrl_mode(void)               { return s_mode; }
uint32_t ctrl_tick_ms(void)           { return s_tick_ms; }

void ctrl_note_cmd(void) { s_cmd_ms = HAL_GetTick(); }

void ctrl_init(void)
{
    s_p.speed_kp    = CFG_SPEED_KP;
    s_p.speed_ki    = CFG_SPEED_KI;
    s_p.speed_ff    = CFG_SPEED_FF;
    s_p.steer_kp    = CFG_STEER_KP;
    s_p.steer_kd    = CFG_STEER_KD;
    s_p.v_straight  = CFG_V_STRAIGHT;
    s_p.v_corner    = CFG_V_CORNER;
    s_p.rate_kp     = CFG_RATE_KP;
    s_p.yaw_hold_kp = CFG_YAW_HOLD_KP;
    s_p.v_max       = CFG_V_MAX;
    s_p.vis_kff     = CFG_VIS_KFF;
    s_p.magic       = CAR_PARAMS_MAGIC;

    pid_init(&s_pid_l, CFG_SPEED_KP, CFG_SPEED_KI, 0.0f, -CFG_PWM_MAX, CFG_PWM_MAX);
    pid_init(&s_pid_r, CFG_SPEED_KP, CFG_SPEED_KI, 0.0f, -CFG_PWM_MAX, CFG_PWM_MAX);
    /* 积分分离：阶跃跟踪时 |err| 大，暂停积分可把超调从 50% 压到 10% 以内 */
    s_pid_l.i_band = CFG_SPEED_I_BAND;
    s_pid_r.i_band = CFG_SPEED_I_BAND;
    lpf1_init(&s_lpf_vl, 1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), CFG_SPEED_LPF_HZ);
    lpf1_init(&s_lpf_vr, 1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), CFG_SPEED_LPF_HZ);
    lpf1_init(&s_lpf_e,  1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), CFG_ERR_LPF_HZ);
    lpf1_init(&s_lpf_gyro, 1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), 50.0f);

    memset(&s_win_l, 0, sizeof(s_win_l));
    memset(&s_win_r, 0, sizeof(s_win_r));

    fsm_init();

    s_v_meas_l = 0.0f;
    s_v_meas_r = 0.0f;
    s_yaw_ref  = 0.0f;
    s_dist_mm  = 0;
    s_dist_acc = 0.0f;
    s_remote_l = 0;
    s_remote_r = 0;
    s_mode     = CAR_MODE_IDLE;
    s_estop    = 0u;
    s_fault    = 0u;
    s_bench    = 0u;
    s_bench_l  = 0.0f;
    s_bench_r  = 0.0f;
    s_fsm_div  = 0u;
    s_tick_ms  = 0u;
    s_track_ok_ms = HAL_GetTick();
    s_cmd_ms   = HAL_GetTick();
    s_vbat_ms  = 0u;
    s_vbat_mv  = 0u;

    s_sh.seq = 0u;

    s_el_prev    = 0xFFu;
    s_turn       = 0u;
    s_turn_target = 0.0f;
    s_turn_ms    = 0u;
    s_turn_ok_ms = 0u;
}

void ctrl_start(void)
{
    if (s_estop || s_mode == CAR_MODE_REMOTE) return;
    if (s_mode == CAR_MODE_IDLE) {
        s_mode = CAR_MODE_TRACK;
        fsm_arm();
        s_track_ok_ms = HAL_GetTick();
        s_cmd_ms = HAL_GetTick();
    }
}

void ctrl_stop(void)
{
    fsm_stop();
    s_mode = CAR_MODE_IDLE;
}

void ctrl_estop(void)
{
    s_estop = 1u;
    s_mode  = CAR_MODE_IDLE;
    fsm_stop();
    bsp_motor_force_stop();
}

void ctrl_set_mode(uint8_t mode)
{
    s_mode = mode;
    if (mode == CAR_MODE_TRACK || mode == CAR_MODE_VISION) fsm_arm();
    else if (mode == CAR_MODE_IDLE) fsm_stop();
}

void ctrl_set_remote_speed(int16_t mm_s_l, int16_t mm_s_r)
{
    s_remote_l = mm_s_l;
    s_remote_r = mm_s_r;
    s_cmd_ms = HAL_GetTick();
}

void ctrl_calib_imu(void)
{
    bsp_imu_zero();
    s_yaw_ref = 0.0f;
}

/* ---------------- 台架模式 ---------------- */

void ctrl_bench(float duty_l, float duty_r)
{
    s_bench_l = clampf(duty_l, -CFG_PWM_MAX, CFG_PWM_MAX);
    s_bench_r = clampf(duty_r, -CFG_PWM_MAX, CFG_PWM_MAX);
    s_bench = 1u;
}

void ctrl_bench_stop(void)
{
    s_bench = 0u;
    s_bench_l = 0.0f;
    s_bench_r = 0.0f;
    bsp_motor_force_stop();
}

uint8_t ctrl_bench_active(void)
{
    return s_bench;
}

/* ---------------- 速度保持模式 ---------------- */

void ctrl_hold_speed(float vl, float vr)
{
    s_hold_l = vl;
    s_hold_r = vr;
    s_hold = 1u;
    s_bench = 0u;               /* 和开环模式互斥 */
}

void ctrl_hold_stop(void)
{
    s_hold = 0u;
    s_hold_l = 0.0f;
    s_hold_r = 0.0f;
    pid_reset(&s_pid_l, 0.0f);
    pid_reset(&s_pid_r, 0.0f);
    bsp_motor_force_stop();
}

uint8_t ctrl_hold_active(void)
{
    return s_hold;
}

/* ---------------- 定角转弯（台架验证用） ---------------- */

/* 以当前朝向为基准转到 yaw+deg（正 = 右转），到位或超时自动停车。
 * 这是 EL_TURN90 的"单步验证器"：先在台架上把 TURN_KP / TOL / 转弯速度调好，
 * 再让状态机去用 —— 避免一上车就在赛道上试错，撞一次可能就报废一个舵机。 */
void ctrl_bench_turn(float deg)
{
    if (deg >  180.0f) deg =  180.0f;
    if (deg < -180.0f) deg = -180.0f;

    s_turn_target = wrap180f(s_imu.yaw_deg + g_yaw_sgn * deg);
    s_turn_ms     = HAL_GetTick();
    s_turn_ok_ms  = 0u;
    s_turn        = 1u;
    s_bench       = 0u;             /* 三种手动模式互斥 */
    s_hold        = 0u;
    s_cmd_ms      = HAL_GetTick();
}

void ctrl_bench_turn_stop(void)
{
    s_turn = 0u;
    bsp_motor_force_stop();
}

uint8_t ctrl_bench_turn_active(void)
{
    return s_turn;
}

/* 运行时改速度环增益：参数副本和 PID 对象**都要**更新。
 * （只改 s_p 的话 pid_step 用的 p->kp/p->ki 还是初始化时的值，改了等于没改。） */
void ctrl_set_speed_gains(float kp, float ki, float ff)
{
    s_p.speed_kp = kp;
    s_p.speed_ki = ki;
    s_p.speed_ff = ff;
    pid_set_gains(&s_pid_l, kp, ki, 0.0f);
    pid_set_gains(&s_pid_r, kp, ki, 0.0f);
}

/* 补线的斜率（err / mm）。
 * 状态机用它区分"弯道"和"断口"：斜率大 = 线在横向扫出去 = 弯道；
 * 斜率≈0 = 线一直居中 = 直道断口。
 * 比"丢线前一刻的误差"可靠得多（那个值随采样时刻随机跳）。 */
float ctrl_pred_slope(void) { return s_pred_slope; }

/* 视觉预告的断口：1 = 前方没有线（盲走区） */
uint8_t ctrl_vis_gap(void) { return s_vis_gap; }

/* 历史窗口内的平均 |误差|。
 * 断线口：线一直居中 → 接近 0
 * 直角弯：线在往外跑 → 0.5 以上（最后两拍的"扫出"会拉高平均）
 * 比单个采样稳定，用来区分断口和弯道。 */
float ctrl_pred_mabs(void)
{
    uint8_t i;
    float   s = 0.0f;
    if (s_ph_n == 0u) return 0.0f;
    for (i = 0u; i < s_ph_n; i++) {
        s += (s_ph_e[i] < 0.0f) ? -s_ph_e[i] : s_ph_e[i];
    }
    return s / (float)s_ph_n;
}

/* 运行时改转向增益（P 与 D）。D 是"预判弯道"的关键项。
 * kp 传 0 表示只改 kd。 */
void ctrl_set_steer(float kp, float kd)
{
    if (kp != 0.0f) s_p.steer_kp = kp;
    s_p.steer_kd = kd;
}

/* 运行时改角速度内环增益。kpHolder 传负值表示不改。 */
void ctrl_set_rate(float rate_kp, float yaw_kp)
{
    if (rate_kp >= 0.0f) s_p.rate_kp = rate_kp;
    if (yaw_kp  >= 0.0f) s_p.yaw_hold_kp = yaw_kp;
}

/* 在线改定角转弯的六个参数。传负值表示"这一项不改"。
 * 注意 v 传 0 是**有效值**（纯原地差速转），不要用 0 表示"不改"。 */
void ctrl_set_turn(float kp, float acc, float deg, float v, float brake_ms, float inner)
{
    if (kp  >= 0.0f) g_turn_kp  = kp;
    if (acc >= 0.0f) g_turn_acc = acc;
    if (deg >= 0.0f) g_turn_deg = deg;
    if (v   >= 0.0f) g_turn_v   = v;
    if (brake_ms >= 0.0f) g_turn_brake = brake_ms;
    if (inner    >= 0.0f) g_turn_inner = inner;
}

void ctrl_get_turn(float *kp, float *acc, float *deg, float *v, float *brake_ms, float *inner)
{
    if (kp)  *kp  = g_turn_kp;
    if (acc) *acc = g_turn_acc;
    if (deg) *deg = g_turn_deg;
    if (v)   *v   = g_turn_v;
    if (brake_ms) *brake_ms = g_turn_brake;
    if (inner)    *inner    = g_turn_inner;
}

/* 直接指定航向参考。锁死盲走用：进盲走那一刻把参考置成绝对目标
 * （发车航向 + 180），之后再靠航向保持把它拉住。 */
void ctrl_set_yaw_ref(float yaw_deg) { s_yaw_ref = yaw_deg; }

/* 台架诊断：把速度环 PID 的内部状态暴露出来。
 * 积分项 i_acc 是关键——如果稳态误差存在而 i_acc 不涨，说明积分被卡住了。 */
float ctrl_pid_iacc_l(void)  { return s_pid_l.i_acc; }
float ctrl_pid_iacc_r(void)  { return s_pid_r.i_acc; }
float ctrl_pid_ilim_l(void)  { return s_pid_l.i_lim; }
void  ctrl_pid_dump_l(char *b, int n)
{
    /* kp / ki / i_acc / out_min / out_max / ref / meas */
    (void)snprintf(b, (size_t)n,
                   "kp=%.2f ki=%.2f iacc=%.0f omin=%.0f omax=%.0f ref=%.1f meas=%.1f",
                   (double)s_pid_l.kp, (double)s_pid_l.ki, (double)s_pid_l.i_acc,
                   (double)s_pid_l.out_min, (double)s_pid_l.out_max,
                   (double)s_hold_l, (double)s_v_meas_l);
}

/* ------------------------------------------------------------------ */

void ctrl_step(void)
{
    const float dt = (float)CFG_CTRL_PERIOD_MS * 0.001f;
    const float fs = 1.0f / dt;
    uint32_t now = HAL_GetTick();
    int16_t  dl = 0, dr = 0;
    float    vl_raw, vr_raw, e, e_f, w_ref, gyro_r, dv, v_cmd;
    float    vl_ref, vr_ref, duty_l, duty_r;
    uint8_t  imu_ok, vis_ok, line_ok, trk_upd = 0u;
    const fsm_out_t *fo;

    s_tick_ms = now;
    s_ticks++;

    /* ---------------- 1. 感知 ----------------
     * 循迹 I2C 只有 25 kHz（再快从机就丢位），一次读取约 1.6 ms。
     * 所以降频到 100 Hz（每 2 拍读一次），和状态机同频，CPU 占用约 16%。
     * 中间那一拍沿用上次的结果，对 100 Hz 的循迹控制完全够用。 */
    if (++s_trk_div >= 2u) {
        int trc;
        s_trk_div = 0u;
        trk_upd = 1u;               /* 本拍真的读了循迹，D 项要按 2 拍算 */
        trc = bsp_track_read(&s_feat);
        s_track_rc = (int8_t)trc;
        if (trc == 0) {
            if (s_feat.valid && !s_feat.junction) s_track_ok_ms = now;
        } else {
            s_feat.valid = 0u;      /* 读失败：当作丢线，绝不能拿旧数据继续控 */
            s_feat.junction = 0u;
        }
    }

    /* 盲走类元素本来就允许长时间没有线（它们靠 IMU 走），
     * 不能算成"丢线故障"，否则盲走区/路口会在半路上被强制停车。
     *
     * 【2026-11-22 修】原来漏了 EL_TURN90 和 EL_SEEK。
     * 直角弯整个动作就是"没有线、靠陀螺仪转"，只要 300 ms 没线就会
     * 触发 FAULT_LINE_LOST → 强制停车，转弯转到一半电机就断电了
     * （实测：yaw 只转了 58~59° 就停，状态从 12 直接跳到 10）。
     * 这两个状态必须一起进白名单。 */
    {
        uint8_t el = (uint8_t)fsm_out()->el;
        if (el == EL_BLIND || el == EL_CROSS || el == EL_TURN90 || el == EL_SEEK ||
            el == EL_LOOP_IN || el == EL_LOOP_RUN || el == EL_LOOP_OUT) {
            s_track_ok_ms = now;
        }
    }

    bsp_encoder_read(&dl, &dr);
    /* 测速窗口：5 ms 内只有 ~10 个脉冲，1 个计数就是 31 mm/s，
     * 这个量化噪声被 P 放大后会形成极限环（实测稳态在 302~366 之间摆）。
     * 用 4 拍（20 ms）滑动窗口把分辨率提高 4 倍。 */
    vl_raw = speed_window(&s_win_l, dl, fs);
    vr_raw = speed_window(&s_win_r, dr, fs);
    s_v_meas_l = lpf1_step(&s_lpf_vl, vl_raw);
    s_v_meas_r = lpf1_step(&s_lpf_vr, vr_raw);
    s_dist_acc += (vl_raw + vr_raw) * 0.5f * dt;
    s_dist_mm   = (int32_t)s_dist_acc;

    imu_ok = (uint8_t)(bsp_imu_get(&s_imu) &&
                       ((uint32_t)(now - s_imu.last_ms) < CFG_IMU_TIMEOUT_MS));
    vis_ok = (uint8_t)(bsp_vision_get(&s_vis) &&
                       ((uint32_t)(now - s_vis.last_ms) < CFG_VIS_TIMEOUT_MS) &&
                       s_vis.valid);
    line_ok = (uint8_t)(s_feat.valid && !s_feat.junction);

    /* 视觉的"断口"预告：最近一帧（且在超时内）说前面没有线。
     * 状态机用它决定"丢线时是直行穿过还是转弯找线"。
     * 视觉断流/超时时这里恒为 0，状态机就退回原来的"转着找线"，不会卡死。 */
    s_vis_gap = (uint8_t)(vis_ok && ((s_vis.flags & VIS_FLAG_KIND_MASK) == VIS_FLAG_GAP));

    /* ---------------- 2. 电池 / 故障 ---------------- */
    if ((uint32_t)(now - s_vbat_ms) >= 100u) {
        s_vbat_ms = now;
        s_vbat_mv = bsp_ui_vbat_mv();
    }

    s_fault = 0u;
    if (s_estop) s_fault |= FAULT_ESTOP;
    /* 只在"看起来真的接了 3S 电池"的区间里判低电压。
     * 台架上没接电池时 PA4 悬空，ADC 会读到随机值；如果刚好落在 0~9.9 V 之间，
     * 就会误判 FAULT_LOW_BATT，把电机锁死 —— 表现为"命令有回显但轮子一动不动"。
     * 低于 6 V 一律视为"没接电池/分压没接"，不报故障。 */
    if (s_vbat_mv >= CFG_VBAT_SENSE_MIN_MV && s_vbat_mv < CFG_VBAT_STOP_MV) {
        s_fault |= FAULT_LOW_BATT;
    }
    if (!imu_ok) s_fault |= FAULT_IMU_LOST;
    if (!vis_ok) s_fault |= FAULT_VIS_LOST;
    if (s_mode == CAR_MODE_REMOTE &&
        (uint32_t)(now - s_cmd_ms) > CFG_FAILSAFE_MS) s_fault |= FAULT_LINK_LOST;
    if (s_mode == CAR_MODE_TRACK &&
        (uint32_t)(now - s_track_ok_ms) > CFG_TRACK_LOST_STOP_MS) s_fault |= FAULT_LINE_LOST;

    /* ---------------- 3. 状态机（每 2 拍 = 10 ms） ---------------- */
    if (++s_fsm_div >= CFG_FSM_DIVIDER) {
        s_fsm_div = 0u;
        fsm_step(&s_feat, &s_imu,
                 (int16_t)((s_v_meas_l + s_v_meas_r) * 0.5f),
                 s_dist_mm, now);
    }
    fo = fsm_out();

    /* 元素切换的瞬间，把"当前朝向"记成新的航向参考。
     * 为什么必须做：s_yaw_ref 平时只在跟线居中时以 0.05 的慢系数跟随，
     * 所以转完 90°、或盲走一段之后，它一定是旧值。等重新锁线、|e| 落进 1.2 以内时，
     * yaw_hold 会突然按 0.8*(yaw - 旧参考) 打一个几十度每秒的偏置 —— 车会猛地一歪。
     * 换状态时重置一次，代价很小，收益是把这个跳变彻底消掉。 */
    if ((uint8_t)fo->el != s_el_prev) {
        s_el_prev = (uint8_t)fo->el;
        /* 写死航向的盲走：参考直接用绝对目标（发车方向 + 180），
         * 而不是"当前朝向"。这样前 7 个直角弯每次差 1° 的累积误差
         * 不会带进 1.5 m 的盲走段。 */
        if (fo->blind_lock) {
            s_yaw_ref = fo->yaw_target;
        } else {
            s_yaw_ref = s_imu.yaw_deg;
        }
    }

    /* ---------------- 3.5 台架模式：直接输出占空比，跳过所有控制 ---------------- */
    if (s_bench && !(s_fault & (FAULT_ESTOP | FAULT_LOW_BATT))) {
        bsp_motor_enable(true);
        bsp_motor_set(s_bench_l, s_bench_r);
        v_cmd = 0.0f;
        w_ref = 0.0f;
        dv    = 0.0f;
        vl_ref = 0.0f;
        vr_ref = 0.0f;
        goto publish;
    }

    /* ---------------- 3.6 速度保持模式：双轮速度闭环，转向环不参与 ----------------
     * 这是上车前必须过的一关：给定目标速度，看两轮能不能稳定跟住、左右是否一致。
     * 不涉及循迹/IMU，纯粹验证「目标速度 → PID → PWM → 编码器」这条闭环。 */
    if (s_hold && !(s_fault & (FAULT_ESTOP | FAULT_LOW_BATT))) {
        bsp_motor_enable(true);
        if (s_hold_l < 1.0f && s_hold_l > -1.0f) {
            pid_reset(&s_pid_l, s_v_meas_l);
            duty_l = 0.0f;
        } else {
            duty_l = s_p.speed_ff * s_hold_l + pid_step(&s_pid_l, s_hold_l, s_v_meas_l, dt);
        }
        if (s_hold_r < 1.0f && s_hold_r > -1.0f) {
            pid_reset(&s_pid_r, s_v_meas_r);
            duty_r = 0.0f;
        } else {
            duty_r = s_p.speed_ff * s_hold_r + pid_step(&s_pid_r, s_hold_r, s_v_meas_r, dt);
        }
        bsp_motor_set(duty_l, duty_r);
        v_cmd = 0.0f;
        w_ref = 0.0f;
        dv    = 0.0f;
        vl_ref = s_hold_l;
        vr_ref = s_hold_r;
        goto publish;
    }

    /* ---------------- 3.7 定角转弯（台架 u 命令） ----------------
     * 和 s_bench / s_hold 一样是"绕过循迹"的手动模式，但走的是同一条
     *     角度环 → 角速度环 → 差速 → 速度环
     * 链路，所以这里测出来的就是 EL_TURN90 上车后会用的东西。
     * 到位自动停、超时也停，绝不原地打转。 */
    if (s_turn && (s_fault & (FAULT_ESTOP | FAULT_LOW_BATT))) {
        s_turn = 0u;                /* 故障优先：转弯立刻放弃 */
    }
    if (s_turn) {
        float gy  = imu_ok ? -lpf1_step(&s_lpf_gyro, s_imu.gyro_dps) : 0.0f;
        float dh  = wrap180f(s_turn_target - s_imu.yaw_deg);
        float w   = turn_rate_cmd(dh, g_yaw_sgn);
        float dvt = clampf(CFG_RATE_FF * w + s_p.rate_kp * (w - gy),
                           -CFG_DV_MAX, CFG_DV_MAX);

        bsp_motor_enable(true);
        vl_ref = clampf(g_turn_v + dvt, -s_p.v_max, s_p.v_max);
        vr_ref = clampf(g_turn_v - dvt, -s_p.v_max, s_p.v_max);
        duty_l = s_p.speed_ff * vl_ref + pid_step(&s_pid_l, vl_ref, s_v_meas_l, dt);
        duty_r = s_p.speed_ff * vr_ref + pid_step(&s_pid_r, vr_ref, s_v_meas_r, dt);
        bsp_motor_set(duty_l, duty_r);

        /* 到位：角度进容差 且 角速度降下来（同 ctrl_fsm 的 EL_TURN90） */
        if ((fabsf(dh) < CFG_TURN_TOL_DEG) && (fabsf(gy) < CFG_TURN_STOP_DPS)) {
            if (s_turn_ok_ms == 0u) s_turn_ok_ms = now;
            if ((uint32_t)(now - s_turn_ok_ms) >= CFG_TURN_SETTLE_MS) s_turn = 0u;
        } else {
            s_turn_ok_ms = 0u;
        }
        if ((uint32_t)(now - s_turn_ms) > CFG_TURN_TIMEOUT_MS) s_turn = 0u;

        if (!s_turn) {              /* 到位 / 超时：停车收工 */
            bsp_motor_force_stop();
            pid_reset(&s_pid_l, 0.0f);
            pid_reset(&s_pid_r, 0.0f);
            vl_ref = 0.0f; vr_ref = 0.0f;
            duty_l = 0.0f; duty_r = 0.0f;
            w      = 0.0f;
            dvt    = 0.0f;
        }

        v_cmd = 0.0f;
        w_ref = w;
        dv    = dvt;
        goto publish;
    }

    /* ---------------- 4. 目标速度 ---------------- */
    if (s_mode == CAR_MODE_TRACK || s_mode == CAR_MODE_VISION) {
        v_cmd = fo->v_cmd;
    } else if (s_mode == CAR_MODE_REMOTE) {
        v_cmd = 0.0f;
    } else {
        v_cmd = 0.0f;
    }

    /* ---------------- 5. 转向：位置环 / 角度环 ----------------
     *
     * 【补线】线消失时，用最后几个有效点外推出线应该在哪，而不是简单地
     * 保持（E_HOLD）或清零。这是智能车竞赛的通用做法，好处是**一套逻辑
     * 同时解决断口和弯道**：
     *   · 直道断口：消失前误差一直居中、斜率≈0 → 外推≈0 → 直行
     *   · 直角弯  ：消失前误差快速变大、斜率很大 → 外推迅速饱和 → 一直转到看见线
     * 所以不需要再"判断这是直角弯还是断口"。
     *
     * 外推是**按里程**算的（err 对 mm 的斜率），不是按时间，
     * 所以车速变了外推的几何形状不变。
     */
    if (line_ok) {
        /* 写入历史并算斜率（最老 -> 最新） */
        uint8_t k;
        for (k = 0u; k + 1u < CFG_PRED_HIST; k++) {
            s_ph_e[k] = s_ph_e[k + 1u];
            s_ph_d[k] = s_ph_d[k + 1u];
        }
        s_ph_e[CFG_PRED_HIST - 1u] = s_feat.err;
        s_ph_d[CFG_PRED_HIST - 1u] = s_dist_mm;
        if (s_ph_n < CFG_PRED_HIST) s_ph_n++;

        if (s_ph_n >= 3u) {
            int32_t dd = s_ph_d[CFG_PRED_HIST - 1u] - s_ph_d[0];
            if (dd >= (int32_t)CFG_PRED_MIN_MM) {
                s_pred_slope = (s_ph_e[CFG_PRED_HIST - 1u] - s_ph_e[0]) / (float)dd;
            } else {
                s_pred_slope = 0.0f;      /* 里程太短，斜率不可信 */
            }
        }
        s_e_last    = s_feat.err;
        s_e_last_ms = now;
        e = s_feat.err;
    } else if ((uint32_t)(now - s_e_last_ms) < CFG_PRED_TIMEOUT_MS) {
        /* 外推：误差 = 最后一次 + 斜率 × 走过的里程 */
        float pred = s_e_last + s_pred_slope * (float)(s_dist_mm - s_e_last_d0);
        if (pred >  CFG_PRED_MAX) pred =  CFG_PRED_MAX;
        if (pred < -CFG_PRED_MAX) pred = -CFG_PRED_MAX;
        e = pred;
    } else {
        e = 0.0f;                         /* 外推超时：交给航向保持/盲走 */
    }
    if (line_ok) s_e_last_d0 = s_dist_mm; /* 记住"最后一次看到线时的里程" */
    e += fo->e_bias;
    e_f = lpf1_step(&s_lpf_e, e);

    if (fo->w_mode == 1u && imu_ok) {
        /* 角度环（直角弯）：完全不用 8 路位置项，直接按航向误差给角速度目标。
         * 这一支必须**跳过** PD、视觉前馈、航向保持 —— 三个源同时往里加会互相打架，
         * 而且此时车本来就看不到线（line_ok=0），循迹项全是噪声。 */
        float dh_t = wrap180f(fo->yaw_target - s_imu.yaw_deg);

        w_ref    = turn_rate_cmd(dh_t, g_yaw_sgn);
        s_e_prev = e_f;         /* 保持微分历史连续，退出角度环那一刻不会打突 */
    } else {
        /* PD：比例项负责"纠偏"，微分项负责"预判"。
         * 【缺陷④修正】循迹是 100 Hz 读的（每 2 拍），如果每拍都求导，
         * 导数会在"更新拍"出现尖峰、"空拍"为 0，产生 100 Hz 的方向抖动。
         * 所以只在**真正读到新数据的拍**更新微分，并按 2 拍的时间间隔算速率。 */
        {
            if (trk_upd) {
                s_de    = (e_f - s_e_prev) / (dt * (float)CFG_FSM_DIVIDER);
                s_e_prev = e_f;
            }
            w_ref = fo->kp_scale * (s_p.steer_kp * e_f +
                                    (line_ok ? s_p.steer_kd * s_de : 0.0f));
        }
        w_ref = clampf(w_ref, -CFG_STEER_KP_MAX, CFG_STEER_KP_MAX);

        /* 视觉前馈：x>0 目标在右 → 想右转 → w 应为正 */
        if (vis_ok) {
            float xn = (float)s_vis.x / 1000.0f;
            w_ref += s_p.steer_kp * 3.5f * xn * s_p.vis_kff;
        }

        /* 航向保持：w = sgn * K * (ref - yaw)，是负反馈。
         * 丢线时也允许保持（盲走段就靠它），但只在"看到线且居中"时才更新参考，
         * 否则盲走段会把参考慢慢拉成当前朝向，等于没有保持。
         *
         * 2026-11-22 修正：原来写的是 dh=(yaw-ref) 且不带符号，
         * 只在"右转时 yaw 变小"的模块约定下才是负反馈；约定相反时变成正反馈，
         * 车会朝一边跑飞 —— 这正是"盲走一走就偏"的典型症状。 */
        if (imu_ok && fo->yaw_hold &&
            (fo->yaw_hard || !line_ok || fabsf(e) < CFG_YAW_HOLD_ERR_MAX)) {
            float dh = wrap180f(s_yaw_ref - s_imu.yaw_deg);
            w_ref += g_yaw_sgn * s_p.yaw_hold_kp * dh;
            /* 参考缓慢跟随当前朝向，所以是 -= （dh 已反向）。
             * 硬保持模式**不跟随**：否则参考会被漂移一路拖走，等于没有保持。 */
            if (!fo->yaw_hard && line_ok && fabsf(e) < CFG_YAW_HOLD_ERR_MAX) {
                s_yaw_ref -= 0.05f * dh;
            }
        }

        w_ref = clampf(w_ref, -CFG_W_MAX, CFG_W_MAX);
    }

    /* ---------------- 6. 角速度环 ---------------- */
    if (imu_ok) {
        /* HWT101 逆时针为正；全工程约定"右转为正"，所以取反。
     * 如果实测符号相反（转动车头向右时 gyro_r 变负），把 CFG_GYRO_INVERT 改成 1。
     * 符号错了这一环就变成正反馈，会直接暴振 —— 调 rate_kp 之前必须先确认这个。 */
#if CFG_GYRO_INVERT
    gyro_r =  lpf1_step(&s_lpf_gyro, s_imu.gyro_dps);
#else
    gyro_r = -lpf1_step(&s_lpf_gyro, s_imu.gyro_dps);
#endif
    } else {
        /* IMU 丢了：退化为开环角速度，靠视觉/循迹慢慢纠正，绝不猛打 */
        gyro_r = 0.0f;
    }

    dv = CFG_RATE_FF * w_ref + s_p.rate_kp * (w_ref - gyro_r);
    dv = clampf(dv, -CFG_DV_MAX, CFG_DV_MAX);

    /* ---------------- 7. 速度环 ---------------- */
    if (s_mode == CAR_MODE_REMOTE) {
        vl_ref = (float)s_remote_l;
        vr_ref = (float)s_remote_r;
    } else {
        /* 【内轮倍率】w_mode=1（定角转弯）时，内轮那一侧再多给 g_turn_inner 倍的差速。
         *
         * 为什么需要：v=0 时两轮目标严格对称（+dv / -dv），但内轮是"逆向走"，
         * 要克服整车旋转惯量 + 静摩擦 + 减速箱反向间隙，实际转速往往跟不上，
         * 表现就是"绕外轮转"，车心画一段小圆弧。
         * 给内轮额外倍率等于单独给它加力矩，让它真正咬住。
         *
         * 代价：两轮不再对称，平均速度 = v - dv*(inner-1)/2 ≠ v，会产生一点
         * 后退分量（车心朝车尾方向偏）。inner 越大这个分量越大，所以别超过 1.5。
         * 默认 1.0 = 与原来完全一致。 */
        float dvo = dv;                    /* 外轮那一侧的差速 */
        float dvi = dv;                    /* 内轮那一侧的差速 */

        if (fo->w_mode == 1u && g_turn_inner > 1.0f) {
            dvi = dv * g_turn_inner;
            if (dvi >  CFG_DV_MAX) dvi =  CFG_DV_MAX;
            if (dvi < -CFG_DV_MAX) dvi = -CFG_DV_MAX;
        }
        /* 用 w_ref 的符号判断"往哪边转"，不要用 dv —— dv 含陀螺仪反馈项，
         * 极端滞后时符号可能瞬时反向，会把内外轮搞反。 */
        if (w_ref >= 0.0f) {               /* 右转：左外右内 */
            vl_ref = v_cmd + dvo;
            vr_ref = v_cmd - dvi;
        } else {                           /* 左转：右外左内 */
            vl_ref = v_cmd + dvi;
            vr_ref = v_cmd - dvo;
        }
    }
    vl_ref = clampf(vl_ref, -s_p.v_max, s_p.v_max);
    vr_ref = clampf(vr_ref, -s_p.v_max, s_p.v_max);

    if (s_mode == CAR_MODE_IDLE || s_mode == CAR_MODE_TEST ||
        (s_fault & (FAULT_ESTOP | FAULT_LOW_BATT | FAULT_LINE_LOST)) ||
        /* 直角弯整个动作都靠陀螺仪，IMU 一丢就没有任何反馈了 ——
         * 此时按 w_mode 分支会退化成"w_ref=0"，车会直着冲出去。
         * 宁可停车，也不要在盲转状态下继续跑。 */
        (fo->el == EL_TURN90 && (s_fault & FAULT_IMU_LOST))) {
        bsp_motor_force_stop();
        pid_reset(&s_pid_l, 0.0f);
        pid_reset(&s_pid_r, 0.0f);
        duty_l = 0.0f;
        duty_r = 0.0f;
        vl_ref = 0.0f;
        vr_ref = 0.0f;
        dv = 0.0f;
        w_ref = 0.0f;
    } else {
        if (!bsp_motor_is_enabled()) {
            bsp_motor_enable(true);
        }
        duty_l = s_p.speed_ff * vl_ref + pid_step(&s_pid_l, vl_ref, s_v_meas_l, dt);
        duty_r = s_p.speed_ff * vr_ref + pid_step(&s_pid_r, vr_ref, s_v_meas_r, dt);
        bsp_motor_set(duty_l, duty_r);
    }

    /* ---------------- 8. 发布快照 ---------------- */
publish:
    st_pub_begin(&s_sh);
    s_sh.s.speed_l    = (int16_t)s_v_meas_l;
    s_sh.s.speed_r    = (int16_t)s_v_meas_r;
    s_sh.s.yaw_ddeg   = s_imu.yaw_ddeg;
    s_sh.s.gyro_ddps  = s_imu.gyro_ddps;
    s_sh.s.track_mask = s_feat.raw;
    s_sh.s.track_count= s_feat.count;
    s_sh.s.track_raw  = s_feat.raw_phys;
    s_sh.s.track_rc   = s_track_rc;
    s_sh.s.line_err   = s_feat.err;
    s_sh.s.err_lv_x100 = (int16_t)(fsm_err_lv() * 100.0f);
    s_sh.s.line_valid = line_ok;
    s_sh.s.vis_x      = s_vis.x;
    s_sh.s.vis_y      = s_vis.y;
    s_sh.s.vis_valid  = vis_ok;
    s_sh.s.vbat_mv    = s_vbat_mv;
    s_sh.s.mode       = s_mode;
    s_sh.s.fault      = s_fault;
    s_sh.s.element    = (uint8_t)fo->el;
    s_sh.s.v_ref_l    = (int16_t)vl_ref;
    s_sh.s.v_ref_r    = (int16_t)vr_ref;
    s_sh.s.duty_l     = (int16_t)duty_l;
    s_sh.s.duty_r     = (int16_t)duty_r;
    s_sh.s.w_ref_ddps = (int16_t)(w_ref * 10.0f);
    s_sh.s.dv_mm      = (int16_t)dv;
    s_sh.s.dist_mm    = s_dist_mm;
    s_sh.s.tick_ms    = now;
    s_sh.s.ctrl_ticks = s_ticks;
    st_pub_end(&s_sh);
}

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

static car_state_shadow_t s_sh;
static car_params_t       s_p;

static pid_t   s_pid_l;
static pid_t   s_pid_r;
static lpf1_t  s_lpf_vl;
static lpf1_t  s_lpf_vr;
static lpf1_t  s_lpf_e;
static lpf1_t  s_lpf_gyro;

static track_feat_t s_feat;
static imu_t        s_imu;
static vision_t     s_vis;

static float    s_v_meas_l;
static float    s_v_meas_r;
static float    s_yaw_ref;
static int32_t  s_dist_mm;
static int16_t  s_remote_l;
static int16_t  s_remote_r;

static uint8_t  s_mode;
static uint8_t  s_estop;
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

static float wrap180f(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
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
    s_p.rate_kp     = CFG_RATE_KP;
    s_p.yaw_hold_kp = CFG_YAW_HOLD_KP;
    s_p.v_max       = CFG_V_MAX;
    s_p.vis_kff     = CFG_VIS_KFF;
    s_p.magic       = CAR_PARAMS_MAGIC;

    pid_init(&s_pid_l, CFG_SPEED_KP, CFG_SPEED_KI, 0.0f, -CFG_PWM_MAX, CFG_PWM_MAX);
    pid_init(&s_pid_r, CFG_SPEED_KP, CFG_SPEED_KI, 0.0f, -CFG_PWM_MAX, CFG_PWM_MAX);
    lpf1_init(&s_lpf_vl, 1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), CFG_SPEED_LPF_HZ);
    lpf1_init(&s_lpf_vr, 1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), CFG_SPEED_LPF_HZ);
    lpf1_init(&s_lpf_e,  1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), CFG_ERR_LPF_HZ);
    lpf1_init(&s_lpf_gyro, 1.0f / ((float)CFG_CTRL_PERIOD_MS * 0.001f), 50.0f);

    fsm_init();

    s_v_meas_l = 0.0f;
    s_v_meas_r = 0.0f;
    s_yaw_ref  = 0.0f;
    s_dist_mm  = 0;
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

/* ------------------------------------------------------------------ */

void ctrl_step(void)
{
    const float dt = (float)CFG_CTRL_PERIOD_MS * 0.001f;
    const float fs = 1.0f / dt;
    uint32_t now = HAL_GetTick();
    int16_t  dl = 0, dr = 0;
    float    vl_raw, vr_raw, e, e_f, w_ref, gyro_r, dv, v_cmd;
    float    vl_ref, vr_ref, duty_l, duty_r;
    uint8_t  imu_ok, vis_ok, line_ok;
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
     * 不能算成"丢线故障"，否则盲走区/路口会在半路上被强制停车。 */
    {
        uint8_t el = (uint8_t)fsm_out()->el;
        if (el == EL_BLIND || el == EL_CROSS ||
            el == EL_LOOP_IN || el == EL_LOOP_RUN || el == EL_LOOP_OUT) {
            s_track_ok_ms = now;
        }
    }

    bsp_encoder_read(&dl, &dr);
    vl_raw = bsp_encoder_delta_mm(dl) * fs;
    vr_raw = bsp_encoder_delta_mm(dr) * fs;
    s_v_meas_l = lpf1_step(&s_lpf_vl, vl_raw);
    s_v_meas_r = lpf1_step(&s_lpf_vr, vr_raw);
    s_dist_mm += (int32_t)((vl_raw + vr_raw) * 0.5f * dt);

    imu_ok = (uint8_t)(bsp_imu_get(&s_imu) &&
                       ((uint32_t)(now - s_imu.last_ms) < CFG_IMU_TIMEOUT_MS));
    vis_ok = (uint8_t)(bsp_vision_get(&s_vis) &&
                       ((uint32_t)(now - s_vis.last_ms) < CFG_VIS_TIMEOUT_MS) &&
                       s_vis.valid);
    line_ok = (uint8_t)(s_feat.valid && !s_feat.junction);

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

    /* ---------------- 4. 目标速度 ---------------- */
    if (s_mode == CAR_MODE_TRACK || s_mode == CAR_MODE_VISION) {
        v_cmd = fo->v_cmd;
    } else if (s_mode == CAR_MODE_REMOTE) {
        v_cmd = 0.0f;
    } else {
        v_cmd = 0.0f;
    }

    /* ---------------- 5. 转向：位置环 ---------------- */
    /* 偏置在丢线时仍然保留：环岛/路口的"盲走段"就是靠它保持方向 */
    e = (line_ok ? s_feat.err : 0.0f) + fo->e_bias;
    e_f = lpf1_step(&s_lpf_e, e);

    w_ref = fo->kp_scale * s_p.steer_kp * e_f;     /* e>0 = 线在右 → w>0 = 右转 */
    w_ref = clampf(w_ref, -CFG_STEER_KP_MAX, CFG_STEER_KP_MAX);

    /* 视觉前馈：x>0 目标在右 → 想右转 → w 应为正 */
    if (vis_ok) {
        float xn = (float)s_vis.x / 1000.0f;
        w_ref += s_p.steer_kp * 3.5f * xn * s_p.vis_kff;
    }

    /* 航向保持：dh>0 表示车头比参考更偏左 → 需要右转 → w>0。
     * 丢线时也允许保持（路口的盲走段就靠它），但只在"看到线且居中"时才更新参考，
     * 否则盲走段会把参考慢慢拉成当前朝向，等于没有保持。 */
    if (imu_ok && fo->yaw_hold && (!line_ok || fabsf(e) < CFG_YAW_HOLD_ERR_MAX)) {
        float dh = wrap180f(s_imu.yaw_deg - s_yaw_ref);
        w_ref += s_p.yaw_hold_kp * dh;
        if (line_ok && fabsf(e) < CFG_YAW_HOLD_ERR_MAX) {
            s_yaw_ref += 0.05f * dh;
        }
    }

    w_ref = clampf(w_ref, -CFG_W_MAX, CFG_W_MAX);

    /* ---------------- 6. 角速度环 ---------------- */
    if (imu_ok) {
        gyro_r = -lpf1_step(&s_lpf_gyro, s_imu.gyro_dps);
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
        vl_ref = v_cmd + dv;
        vr_ref = v_cmd - dv;
    }
    vl_ref = clampf(vl_ref, -s_p.v_max, s_p.v_max);
    vr_ref = clampf(vr_ref, -s_p.v_max, s_p.v_max);

    if (s_mode == CAR_MODE_IDLE || s_mode == CAR_MODE_TEST ||
        (s_fault & (FAULT_ESTOP | FAULT_LOW_BATT | FAULT_LINE_LOST))) {
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
    s_sh.s.dist_mm    = s_dist_mm;
    s_sh.s.tick_ms    = now;
    s_sh.s.ctrl_ticks = s_ticks;
    st_pub_end(&s_sh);
}

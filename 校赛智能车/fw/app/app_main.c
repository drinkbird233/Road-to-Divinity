/**
 * @file  app_main.c
 * @brief 初始化 + 任务创建 + TIM6 节拍中断。
 *
 * 任务模型（三任务，够用且好调）：
 *   ctrl  优先级 5  TIM6 通知      5 ms   控制 + 状态机 + 按键扫描
 *   comm  优先级 3  自身延时       2 ms   串口解析 + 遥测 20 ms
 *   ui    优先级 1  自身延时     100 ms   OLED / LED / 蜂鸣器
 *
 * 【重要】HAL 时基必须是 TIM7，TIM6 留给控制节拍。
 * 如果你用了 CubeMX 生成的 stm32f4xx_hal_timebase_tim.c，
 * 请把它里面的 HAL_TIM_PeriodElapsedCallback 删掉，用本文件的版本。
 */
#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "app_main.h"
#include "app_config.h"
#include "app_types.h"

#include "bsp_motor.h"
#include "bsp_encoder.h"
#include "bsp_track.h"
#include "bsp_imu.h"
#include "bsp_vision.h"
#include "bsp_uart.h"
#include "bsp_ui.h"

#include "ctrl_core.h"
#include "ctrl_fsm.h"
#include "proto.h"

#include <stdio.h>
#include <stdarg.h>

#define PACKET_MAX   (PROTO_MAX_PAYLOAD + 5u)

/* CubeMX 生成的外设句柄 */
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart6;
extern TIM_HandleTypeDef  htim6;

/* ---------------- 串口实例 ---------------- */
static bsp_uart_t s_u_dbg;
static bsp_uart_t s_u_imu;
static bsp_uart_t s_u_vis;
static bsp_uart_t s_u_bt;

static TaskHandle_t s_task_ctrl;

/* ================= 调试输出 ================= */

void app_log(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        /* 同时送调试口和蓝牙。
         * 现场经常只接了 HC06、没接 USB-TTL，所以日志必须两路都发。
         * UART 发送是纯移位，没人接收也不会阻塞，这里多点开销可以忽略。 */
        (void)bsp_uart_send_blocking(&s_u_dbg, (const uint8_t *)buf, (uint16_t)n);
        (void)bsp_uart_send_blocking(&s_u_bt,  (const uint8_t *)buf, (uint16_t)n);
    }
}

/* printf 重定向。
 * Keil（AC5/AC6）用的是 fputc；arm-none-eabi（GCC/newlib）用的是 __io_putchar。
 * 注意：AC6 同时定义了 __GNUC__，所以必须先判 __ARMCC_VERSION，否则在 Keil 下
 * 会去定义 __io_putchar —— 编译能过，但 printf 一个字都出不来，而且不报错。 */
#if defined(__ARMCC_VERSION)
int fputc(int ch, FILE *f)
{
    uint8_t c = (uint8_t)ch;
    (void)f;
    (void)bsp_uart_send_blocking(&s_u_dbg, &c, 1u);
    return ch;
}
#else
int __io_putchar(int ch)
{
    uint8_t c = (uint8_t)ch;
    (void)bsp_uart_send_blocking(&s_u_dbg, &c, 1u);
    return ch;
}
#endif

/* ================= 遥测 / 命令 ================= */

static uint8_t bt_connected(void)
{
    return (HAL_GPIO_ReadPin(BT_STATE_GPIO_Port, BT_STATE_Pin) == GPIO_PIN_SET) ? 1u : 0u;
}

static void send_telemetry(void)
{
    uint8_t pl[17];
    uint8_t fr[PACKET_MAX];
    uint8_t i;
    uint16_t n;
    car_state_t st;
    uint8_t ok = 0u;

    for (i = 0; i < 3u; i++) {
        if (st_snap(ctrl_shadow(), &st)) { ok = 1u; break; }
    }
    if (!ok) return;

    put_u16(&pl[0],  (uint16_t)st.tick_ms);
    put_i16(&pl[2],  st.speed_l);
    put_i16(&pl[4],  st.speed_r);
    put_i16(&pl[6],  st.yaw_ddeg);
    put_i16(&pl[8],  st.gyro_ddps);
    pl[10] = st.track_mask;
    put_u16(&pl[11], st.vbat_mv);
    pl[13] = (uint8_t)(st.vis_x / 10);     /* ±1000 → ±100，符合协议的 int8 缩放 */
    pl[14] = (uint8_t)(st.vis_y / 10);
    pl[15] = st.mode;
    pl[16] = (uint8_t)((st.fault & FAULT_IMU_LOST ? 0u : 0x01u) |
                       (st.vis_valid ? 0x02u : 0u) |
                       (bt_connected() ? 0x04u : 0u) |
                       ((st.vbat_mv > 0u && st.vbat_mv < CFG_VBAT_WARN_MV) ? 0x08u : 0u));

    n = proto_build(fr, sizeof(fr), PROTO_T_TELEMETRY, pl, 17u);
    if (n > 0u) (void)bsp_uart_send(&s_u_bt, fr, n);
}

static void send_ack(uint8_t cmd, uint8_t result)
{
    uint8_t pl[2];
    uint8_t fr[PACKET_MAX];
    uint16_t n;

    pl[0] = cmd;
    pl[1] = result;
    n = proto_build(fr, sizeof(fr), PROTO_T_ACK, pl, 2u);
    if (n > 0u) (void)bsp_uart_send(&s_u_bt, fr, n);
}

/* cmd 帧 payload: [0] = cmd, 后面是参数（小端） */
static void on_command(uint8_t type, const uint8_t *pl, uint8_t len, void *user)
{
    uint8_t cmd, res = 0u;
    (void)user;

    if (type != PROTO_T_COMMAND || len < 1u) return;

    cmd = pl[0];
    ctrl_note_cmd();

    switch (cmd) {
    case CMD_START:
        ctrl_start();
        break;
    case CMD_STOP:
        ctrl_stop();
        break;
    case CMD_SET_SPEED:
        if (len >= 5u) ctrl_set_remote_speed(get_i16(&pl[1]), get_i16(&pl[3]));
        else res = 2u;
        break;
    case CMD_SET_PID:
        /* [1]=loop  [2..3]=kp/100  [4..5]=ki/100  [6..7]=kd/100 */
        if (len >= 4u) {
            car_params_t *p = ctrl_params();
            float kp = (float)get_i16(&pl[2]) / 100.0f;
            float ki = (len >= 6u) ? (float)get_i16(&pl[4]) / 100.0f : p->speed_ki;
            switch (pl[1]) {
            case 0u: p->speed_kp = kp; p->speed_ki = ki; break;
            case 1u: p->steer_kp = kp; break;
            case 2u: p->rate_kp  = kp; break;
            case 3u: p->yaw_hold_kp = kp; break;
            default: res = 3u; break;
            }
        } else {
            res = 2u;
        }
        break;
    case CMD_SET_MODE:
        if (len >= 2u) ctrl_set_mode(pl[1]);
        else res = 2u;
        break;
    case CMD_REQ_TELEMETRY:
        send_telemetry();
        break;
    case CMD_CALIB_IMU:
        ctrl_calib_imu();
        break;
    case CMD_SET_STREAM_RATE:
        /* 固定 50 Hz，先占位不实现 */
        break;
    default:
        res = 1u;
        break;
    }

    send_ack(cmd, res);
}

static proto_rx_t s_bt_parser;

static void bt_feed(const uint8_t *d, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) proto_rx_feed(&s_bt_parser, d[i], on_command, 0);
}

/* ================= 台架命令行（USART1 / 蓝牙同时可用） ================= */

static char    s_cli[64];
static uint8_t s_cli_len;
static uint8_t s_cli_seen;      /* 收到过至少一条命令后置位，用于关掉心跳点 */
static uint32_t s_cli_ms;       /* 最后一次收到字节的时刻 */
static uint8_t s_watch;         /* w 命令：编码器/速度连续监控 */
static uint8_t s_trk_watch;     /* d 命令：循迹 8 位二进制连续显示 */
static uint8_t s_keytest;       /* b 命令：按键识别模式（只报告不动作） */
static uint8_t s_speed_trk;     /* o 命令：50 ms 速度曲线 */
static uint8_t s_telem_on;      /* y 命令：遥测帧开关（默认关） */

/* 把字节按 L1..L8 顺序（bit0 在前）格式化成 8 位。
 * 实测这块模块是 L1 = bit0（手册写的 X1 = bit7 是反的）。
 * bits_raw：原始电平，0 = 压黑线，1 = 白。
 * bits_lit：取反后，1 = 压黑线（"亮灯"），更符合直觉。 */
static void bits_raw(char *out, uint8_t v)
{
    uint8_t i;
    for (i = 0; i < 8u; i++) {
        out[i] = ((v >> i) & 1u) ? '1' : '0';
    }
    out[8] = '\0';
}

/* 把"哪几路压到黑线"翻译成名字。传入的是 mask（bit i = L(i+1)，1 = 压黑线）。 */
static void name_hit(char *out, uint8_t mask)
{
    uint8_t i, n = 0u;
    for (i = 0; i < 8u; i++) {
        if (((mask >> i) & 1u) != 0u) {
            out[n++] = 'L';
            out[n++] = (char)('1' + i);
            out[n++] = ' ';
        }
    }
    if (n == 0u) {
        out[n++] = 'n'; out[n++] = 'o'; out[n++] = 'n'; out[n++] = 'e';
    }
    out[n] = '\0';
}

static void encoder_pin_probe(void);   /* 定义在下面，先声明 */

static void cli_help(void)
{
    app_log("\r\n[cli] h            help\r\n");
    app_log("[cli] m <l> <r>    motor duty -8399..8399\r\n");
    app_log("[cli] r <l> <r>    speed hold mm/s (closed loop)\r\n");
    app_log("[cli] x            stop / leave bench\r\n");
    app_log("[cli] e            encoder raw counters\r\n");
    app_log("[cli] k            encoder timer registers\r\n");
    app_log("[cli] w            toggle encoder watch (200 ms)\r\n");
    app_log("[cli] n            probe encoder pins for 1 s (turn wheel!)\r\n");
    app_log("[cli] s            swap track SCL/SDA and probe 0x12\r\n");
    app_log("[cli] q            measure imu frame rate\r\n");
    app_log("[cli] y            toggle telemetry frames\r\n");
    app_log("[cli] f            scan track i2c registers 0x00-0x3F\r\n");
    app_log("[cli] t            track mask / err\r\n");
    app_log("[cli] d            track 8-bit continuous display\r\n");
    app_log("[cli] g <kp> <ki> <ff>  set speed gains (x10)\r\n");
    app_log("[cli] c [kp kd vs vc] steering gains (x10) and straight/corner speeds\r\n");
    app_log("[cli] o            speed trace 50 ms\r\n");
    app_log("[cli] b            key identify mode (report only, no action)\r\n");
    app_log("[cli] a            start (enter TRACK mode, same as start key)\r\n");
    app_log("[cli] z <us>       set track i2c half period\r\n");
    app_log("[cli] i            imu yaw / gyro\r\n");
    app_log("[cli] v            car state\r\n");
    app_log("[cli] p            motor / pin / timer state\r\n");
}

static int32_t cli_num(const char **pp)
{
    const char *p = *pp;
    int32_t v = 0;
    int8_t  sign = 1;

    while (*p == ' ') p++;
    if (*p == '-')      { sign = -1; p++; }
    else if (*p == '+') { p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return v * sign;
}

static const char *cli_skip(const char *p)
{
    while (*p == ' ') p++;
    return p;
}

static void cli_exec(void)
{
    const char *p = cli_skip(s_cli);
    car_state_t st;

    if (*p == '\0') return;

    s_cli_seen = 1u;            /* 只要收到过一条命令，就停掉串口心跳点 */

    switch (*p) {
    case 'h':
    case '?':
        cli_help();
        break;

    case 'm': {
        int32_t dl, dr;
        p = cli_skip(p + 1);
        dl = cli_num(&p);
        p  = cli_skip(p);
        dr = cli_num(&p);
        ctrl_bench((float)dl, (float)dr);
        app_log("[cli] bench L=%d R=%d\r\n", (int)dl, (int)dr);
        break;
    }

    case 'r': {
        /* 速度保持：r <左 mm/s> <右 mm/s>，例如 r 300 300 */
        int32_t vl, vr;
        p = cli_skip(p + 1);
        vl = cli_num(&p);
        p  = cli_skip(p);
        vr = cli_num(&p);
        ctrl_hold_speed((float)vl, (float)vr);
        app_log("[cli] hold L=%d R=%d mm/s\r\n", (int)vl, (int)vr);
        break;
    }

    case 'a':
        /* 串口启动循迹模式（KEY1 坏了，用串口代替按键启动） */
        ctrl_start();
        app_log("[cli] start (mode TRACK)\r\n");
        break;

    case 'x':
        ctrl_bench_stop();
        ctrl_hold_stop();
        ctrl_stop();
        app_log("[cli] stop\r\n");
        break;

    case 'e':
        app_log("[cli] enc L=%d R=%d\r\n",
                (int)bsp_encoder_raw_l(), (int)bsp_encoder_raw_r());
        break;

    case 'k':
        /* 编码器定时器体检：确认进了编码器模式、计数器在动 */
        app_log("[cli] TIM3(R) CR1=%04X SMCR=%04X CCER=%04X CNT=%u\r\n",
                (unsigned)TIM3->CR1, (unsigned)TIM3->SMCR, (unsigned)TIM3->CCER,
                (unsigned)TIM3->CNT);
        app_log("[cli] TIM4(L) CR1=%04X SMCR=%04X CCER=%04X CNT=%u\r\n",
                (unsigned)TIM4->CR1, (unsigned)TIM4->SMCR, (unsigned)TIM4->CCER,
                (unsigned)TIM4->CNT);
        break;

    case 'n':
        encoder_pin_probe();    /* 1 秒内转轮子，看跳变次数 */
        break;

    case 's':
        /* 切换循迹 SCL/SDA 角色并立即探测 0x12。
         * 哪个 swap 值能探测到 1，就把 app_config.h 里
         * CFG_TRACK_SWAP_SCL_SDA 固化成那个值。 */
        bsp_track_set_swap((uint8_t)(bsp_track_get_swap() ? 0u : 1u));
        app_log("[cli] swap=%u probe 0x%02X = %u\r\n",
                (unsigned)bsp_track_get_swap(),
                (unsigned)CFG_TRACK_ADDR,
                (unsigned)bsp_track_probe(CFG_TRACK_ADDR));
        break;

    case 'f': {
        /* 寄存器扫描：把 0x00~0x3F 里读得到的值全打出来。
         * 用途：模块的字节布局可能和常规不同（比如 8 路数据分在两个字节里，
         * 或者根本不在 0x30），扫一遍就能看出来。 */
        uint8_t a, v;
        int k, hits = 0;
        app_log("[cli] reg scan 0x00-0x3F (addr 0x%02X):\r\n",
                (unsigned)CFG_TRACK_ADDR);
        for (k = 0; k < 64; k++) {
            a = (uint8_t)k;
            if (bsp_track_read_reg(a, &v) == 0) {
                app_log("  %02X=%02X\r\n", (unsigned)a, (unsigned)v);
                hits++;
            }
        }
        app_log("[cli] %d regs readable\r\n", hits);
        break;
    }

    case 'g': {
        /* g <kp×10> <ki×10> <ff×10>：运行时改速度环增益。
         * 不带参数则打印当前值。例：g 65 600 80 → kp=6.5 ki=60.0 ff=8.0 */
        car_params_t *pp = ctrl_params();
        p = cli_skip(p + 1);
        if (*p == '\0') {
            int a = (int)(pp->speed_kp * 10.0f + 0.5f);
            int b = (int)(pp->speed_ki * 10.0f + 0.5f);
            int c = (int)(pp->speed_ff * 10.0f + 0.5f);
            app_log("[cli] kp=%d.%d ki=%d.%d ff=%d.%d\r\n",
                    a / 10, a % 10, b / 10, b % 10, c / 10, c % 10);
        } else {
            int32_t a = cli_num(&p); p = cli_skip(p);
            int32_t b = cli_num(&p); p = cli_skip(p);
            int32_t c = cli_num(&p);
            ctrl_set_speed_gains((float)a / 10.0f, (float)b / 10.0f, (float)c / 10.0f);
            app_log("[cli] set kp=%ld.%ld ki=%ld.%ld ff=%ld.%ld\r\n",
                    (long)(a / 10), (long)(a % 10),
                    (long)(b / 10), (long)(b % 10),
                    (long)(c / 10), (long)(c % 10));
        }
        break;
    }

    case 'b':
        /* 按键识别：板子没丝印时，发 b 后逐个按，看打印的是 KEY1/2/3。
         * 此模式下按键不触发任何动作，安全。 */
        s_keytest = (uint8_t)(s_keytest ? 0u : 1u);
        app_log("[cli] key test %s\r\n", s_keytest ? "ON (keys only report)" : "OFF");
        break;

    case 'c': {
        /* c                              打印当前转向/速度参数
         * c <kp×10> <kd×10> <直道速度> <弯道速度>   在线设置
         * 例：c 70 60 300 250  → kp=7.0 kd=6.0 直道 300 弯道 250 */
        p = cli_skip(p + 1);
        if (*p == '\0') {
            car_params_t *pp = ctrl_params();
            float vs = 0.0f, vc = 0.0f;
            int a, b;
            fsm_get_speeds(&vs, &vc);
            a = (int)(pp->steer_kp * 10.0f + 0.5f);
            b = (int)(pp->steer_kd * 10.0f + 0.5f);
            app_log("[cli] steer kp=%d.%d kd=%d.%d  vs=%d vc=%d\r\n",
                    a / 10, a % 10, b / 10, b % 10, (int)vs, (int)vc);
        } else {
            int32_t a = cli_num(&p); p = cli_skip(p);
            int32_t b = cli_num(&p); p = cli_skip(p);
            int32_t vs = cli_num(&p); p = cli_skip(p);
            int32_t vc = cli_num(&p);
            ctrl_set_steer((float)a / 10.0f, (float)b / 10.0f);
            fsm_set_speeds((float)vs, (float)vc);
            app_log("[cli] set steer kp=%ld.%ld kd=%ld.%ld vs=%ld vc=%ld\r\n",
                    (long)(a / 10), (long)(a % 10), (long)(b / 10), (long)(b % 10),
                    (long)vs, (long)vc);
        }
        break;
    }

    case 'o':
        /* 速度曲线：每 50 ms 一行 "<目标> <左实测> <右实测>"，用于看阶跃响应 */
        s_speed_trk = (uint8_t)(s_speed_trk ? 0u : 1u);
        app_log("[cli] speed trace %s\r\n", s_speed_trk ? "ON" : "OFF");
        break;

    case 'z': {
        /* z <half_us>：改 I2C 半周期。用于排查"读取偏移"是否与时序有关。
         * 遮住 L1，依次 z 2 / z 5 / z 10 / z 20，每次发 t 记 raw。
         * 如果某个速度下 raw 变成 7F（手册值），就是时序问题，能修好。 */
        int32_t us;
        p = cli_skip(p + 1);
        us = cli_num(&p);
        if (us < 1) us = 1;
        if (us > 100) us = 100;
        bsp_track_set_half_us((uint32_t)us);
        app_log("[cli] i2c half=%lu us (~%lu kHz)\r\n",
                (unsigned long)bsp_track_half_us(),
                (unsigned long)(500u / (bsp_track_half_us() ? bsp_track_half_us() : 1u)));
        break;
    }

    case 'q': {
        /* 测 IMU 实际输出频率。
         * 注意：测量期间必须**继续读串口**，否则没人喂解析器，计数不会动
         *（第一版用 vTaskDelay 睡觉，结果永远是 0 Hz —— 那是测量的 bug，不是 IMU 的）。*/
        imu_t a, b;
        uint32_t t0;
        uint8_t qb[32];
        int qn;
        if (!bsp_imu_get(&a)) { app_log("[cli] imu no frame\r\n"); break; }
        t0 = HAL_GetTick();
        while ((uint32_t)(HAL_GetTick() - t0) < 1000u) {
            while ((qn = bsp_uart_read(&s_u_imu, qb, (int)sizeof(qb))) > 0) {
                bsp_imu_feed(qb, (uint16_t)qn);
            }
            vTaskDelay(1);
        }
        (void)bsp_imu_get(&b);
        {
            uint32_t dt = HAL_GetTick() - t0;
            uint32_t dn = b.frames - a.frames;
            if (dt == 0u) dt = 1u;
            app_log("[cli] imu %lu frames / %lu ms = %lu Hz\r\n",
                    (unsigned long)dn, (unsigned long)dt,
                    (unsigned long)((dn * 1000u) / dt));
            app_log("[cli]   0x52 +%lu  0x53 +%lu  crc=%lu\r\n",
                    (unsigned long)(b.n52 - a.n52),
                    (unsigned long)(b.n53 - a.n53),
                    (unsigned long)b.crc_err);
        }
        break;
    }

    case 'w':
        /* 连续监控开关：打开后每 200 ms 打一次编码器和速度，方便双手转轮子 */
        s_watch = (uint8_t)(s_watch ? 0u : 1u);
        app_log("[cli] watch %s\r\n", s_watch ? "ON" : "OFF");
        break;

    case 'y':
        /* 遥测帧开关。开着的时候蓝牙口上会混入二进制帧，日志会变得难读 */
        s_telem_on = (uint8_t)(s_telem_on ? 0u : 1u);
        app_log("[cli] telemetry %s\r\n", s_telem_on ? "ON" : "OFF");
        break;

    case 't':
        /* 只读 ctrl 已经采好的快照，**不再自己碰 I2C**。
         * 两个任务并发操作同一条软 I2C 总线会互相打断，把从机搞成"总线拉死"状态，
         * 进而让最高优先级的控制任务在超时循环里烧掉 CPU，命令行整个卡住。 */
        if (st_snap(ctrl_shadow(), &st)) {
            char br[9], hit[32];
            /* 用 mask（已归一化：bit0 = L1，1 = 压黑线），名字和位串都从它来 */
            bits_raw(br, st.track_mask);
            name_hit(hit, st.track_mask);
            if (st.track_rc == 0) {
                app_log("[cli] hit: %s\r\n", hit);
            } else {
                app_log("[cli] hit: (i2c err %d)\r\n", (int)st.track_rc);
            }
            app_log("[cli] bits=%s cnt=%u err=%d raw=%02X\r\n",
                    br, (unsigned)st.track_count,
                    (int)(st.line_err * 100.0f), (unsigned)st.track_raw);
        }
        break;

    case 'd':
        /* 循迹连续显示：每 100 ms 一行 8 位二进制，方便手拿模块一边扫一边看 */
        s_trk_watch = (uint8_t)(s_trk_watch ? 0u : 1u);
        app_log("[cli] track display %s\r\n", s_trk_watch ? "ON" : "OFF");
        break;

    case 'i':
        if (st_snap(ctrl_shadow(), &st)) {
            int y = st.yaw_ddeg, g = st.gyro_ddps;
            app_log("[cli] yaw=%d.%d gyro=%d.%d\r\n",
                    y / 10, (y < 0 ? -y : y) % 10,
                    g / 10, (g < 0 ? -g : g) % 10);
        }
        {
            imu_t im;
            if (bsp_imu_get(&im)) {
                app_log("[cli] imu 0x52=%lu 0x53=%lu other=%lu crc=%lu\r\n",
                        (unsigned long)im.n52, (unsigned long)im.n53,
                        (unsigned long)im.nother, (unsigned long)im.crc_err);
            } else {
                app_log("[cli] imu no frame\r\n");
            }
        }
        break;

    case 'v':
        if (st_snap(ctrl_shadow(), &st)) {
            app_log("[cli] mode=%u fault=%04X el=%u dist=%ld vbat=%umV ticks=%lu\r\n",
                    (unsigned)st.mode, (unsigned)st.fault,
                    (unsigned)st.element, (long)st.dist_mm, (unsigned)st.vbat_mv,
                    (unsigned long)st.ctrl_ticks);
        }
        /* 蓝牙链路体检：STATE 引脚 + 累计收到的字节数。
         * bt_rx 一直是 0 → 一个字节都没进来（线没接 / 波特率不对 / 没连接）。
         * bt_rx 在涨但你发的命令没反应 → 波特率不对，收到的是乱码。 */
        app_log("[cli] bt_state=%u bt_rx=%lu\r\n",
                (unsigned)((HAL_GPIO_ReadPin(BT_STATE_GPIO_Port, BT_STATE_Pin) == GPIO_PIN_SET) ? 1u : 0u),
                (unsigned long)bsp_uart_rx_total(&s_u_bt));
        break;

    case 'p':
        /* 输出侧体检：软件到底有没有把引脚驱动起来 */
        app_log("[cli] en=%u dutyL=%d dutyR=%d\r\n",
                (unsigned)bsp_motor_is_enabled(),
                (int)bsp_motor_duty_l(), (int)bsp_motor_duty_r());
        app_log("[cli] GPIOE.IDR=%04X  STBY(PE14)=%u IN1_L(PE8)=%u IN2_L(PE10)=%u\r\n",
                (unsigned)GPIOE->IDR,
                (unsigned)((GPIOE->IDR >> 14) & 1u),
                (unsigned)((GPIOE->IDR >> 8)  & 1u),
                (unsigned)((GPIOE->IDR >> 10) & 1u));
        app_log("[cli] TIM1 CR1=%04X CCER=%04X CCR1=%u CCR2=%u\r\n",
                (unsigned)TIM1->CR1, (unsigned)TIM1->CCER,
                (unsigned)TIM1->CCR1, (unsigned)TIM1->CCR2);
        /* 速度环 PID 内部状态：i_acc 是关键，若稳态有误差但它不涨 = 积分被卡住 */
        app_log("[cli] pid iaccL=%d iaccR=%d ilim=%d\r\n",
                (int)ctrl_pid_iacc_l(), (int)ctrl_pid_iacc_r(),
                (int)ctrl_pid_ilim_l());
        {
            char pb[96];
            ctrl_pid_dump_l(pb, (int)sizeof(pb));
            app_log("[cli] %s\r\n", pb);
        }
        break;

    default:
        app_log("[cli] ? for help\r\n");
        break;
    }
}

/* 编码器引脚活动测试：不改引脚模式（AF 下 IDR 依然反映真实电平），
 * 先等 2 秒给人反应，再采样 3 秒统计跳变次数。
 * 同时报告定时器 CNT 的增量，作为交叉验证。 */
static void encoder_pin_probe(void)
{
    uint16_t c3a, c4a, c3b, c4b;
    uint32_t t0;
    uint8_t  last[4], cur[4];
    uint16_t edge[4] = {0u, 0u, 0u, 0u};
    uint8_t  i;

    c3a = (uint16_t)TIM3->CNT;
    c4a = (uint16_t)TIM4->CNT;

    app_log("[cli] probe: TURN THE WHEEL NOW! sampling in 2s\r\n");
    vTaskDelay(pdMS_TO_TICKS(2000u));      /* 留出反应时间 */
    app_log("[cli] sampling...\r\n");

    last[0] = (uint8_t)((GPIOA->IDR >> 6) & 1u);   /* RA = PA6 */
    last[1] = (uint8_t)((GPIOA->IDR >> 7) & 1u);   /* RB = PA7 */
    last[2] = (uint8_t)((GPIOB->IDR >> 6) & 1u);   /* LA = PB6 */
    last[3] = (uint8_t)((GPIOB->IDR >> 7) & 1u);   /* LB = PB7 */

    t0 = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - t0) < 3000u) {
        cur[0] = (uint8_t)((GPIOA->IDR >> 6) & 1u);
        cur[1] = (uint8_t)((GPIOA->IDR >> 7) & 1u);
        cur[2] = (uint8_t)((GPIOB->IDR >> 6) & 1u);
        cur[3] = (uint8_t)((GPIOB->IDR >> 7) & 1u);
        for (i = 0u; i < 4u; i++) {
            if (cur[i] != last[i]) {
                edge[i]++;
                last[i] = cur[i];
            }
        }
    }

    c3b = (uint16_t)TIM3->CNT;
    c4b = (uint16_t)TIM4->CNT;

    app_log("[cli] edges: RA=%u RB=%u LA=%u LB=%u\r\n",
            (unsigned)edge[0], (unsigned)edge[1], (unsigned)edge[2], (unsigned)edge[3]);
    app_log("[cli] CNT delta: R=%d L=%d\r\n",
            (int)(int16_t)(c3b - c3a), (int)(int16_t)(c4b - c4a));
}

static void cli_run(void)
{
    if (s_cli_len > 0u) {
        s_cli[s_cli_len] = '\0';
        app_log(">%s\r\n", s_cli);      /* 回显：先证明"收到了什么"，再谈结果 */
        cli_exec();
        s_cli_len = 0u;
    }
}

static void cli_feed(uint8_t c)
{
    if (c == '\r' || c == '\n') {
        cli_run();
        return;
    }
    if (s_cli_len < (uint8_t)(sizeof(s_cli) - 1u)) {
        s_cli[s_cli_len++] = (char)c;
        s_cli_ms = HAL_GetTick();       /* 记下最后一个字节的时刻 */
    }
}

/* 没有回车也能用：收完 150 ms 内没有新字节，就当一条命令结束 */
static void cli_poll(void)
{
    if (s_cli_len > 0u && (uint32_t)(HAL_GetTick() - s_cli_ms) >= 150u) {
        cli_run();
    }
}

/* ================= 任务 ================= */

static void ctrl_task(void *arg)
{
    uint32_t dbg_n = 0u;
    uint32_t dbg_t = HAL_GetTick();
    (void)arg;

    app_log("\r\n[ctrl alive]\r\n");    /* 存活标记：没有这行 = 控制任务没跑起来 */

    /* TIM6 的 NVIC 抢占优先级在这里再设一次，覆盖 CubeMX 生成的值。
     * 原因：CubeMX 的 per-peripheral NVIC 页常常是只读的，改不动；
     * 而且即使改了，重新生成也可能回退。这里的代码永远不会被覆盖。
     * 必须 >= 5，否则 S6 开 FreeRTOS 后 vTaskNotifyGiveFromISR 会触发 configASSERT。 */
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 6, 0);

    HAL_TIM_Base_Start_IT(&htim6);      /* 调度器已跑起来，此刻起每 5 ms 通知一次 */

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bsp_ui_scan();                   /* 按键：3 × 5 ms = 15 ms 消抖，兼作急停采样 */

        if (s_keytest) {
            /* 按键识别模式：只报告，不执行任何动作（按 KEY1 不会启动，按 KEY3 不会急停）。
             * 板子没有丝印时用这个确认哪个物理键是哪一路。 */
            uint8_t k;
            for (k = 0u; k < 3u; k++) {
                if (bsp_ui_key_pressed(k)) {
                    app_log("[key] KEY%u pressed\r\n", (unsigned)(k + 1u));
                }
            }
        } else {
            if (bsp_ui_key_pressed(CFG_KEY_START_IDX)) {   /* 启动 / 停止 */
                if (ctrl_mode() == CAR_MODE_IDLE) ctrl_start();
                else ctrl_stop();
            }
            if (bsp_ui_key_pressed(CFG_KEY_IMU_IDX)) {     /* IMU 零位 */
                ctrl_calib_imu();
            }
            if (bsp_ui_key_pressed(CFG_KEY_ESTOP_IDX)) {   /* 急停（粘滞） */
                ctrl_estop();
            }
        }

        ctrl_step();

        /* 周期体检：只在**异常**时打印（200 拍应该正好 1000 ms）。
         * 健康时完全静默，减少蓝牙链路的压力，也便于一眼看出问题。 */
        if (++dbg_n >= 200u) {
            uint32_t t = HAL_GetTick();
            uint32_t dt = t - dbg_t;
            dbg_n = 0u;
            dbg_t = t;
            if (dt > 1500u) {
                app_log("[c] SLOW %lu ms/200\r\n", (unsigned long)dt);
            }
        }
    }
}

static void comm_task(void *arg)
{
    uint8_t  buf[64];
    int      n;
    uint32_t next_telem = 0u;
    uint32_t next_health = 0u;
    uint32_t next_beat = 0u;
    uint32_t next_watch = 0u;
    uint32_t next_trkw = 0u;
    uint32_t next_spd = 0u;
    uint32_t next_btdbg = 0u;

    (void)arg;

    app_log("\r\n[comm alive]\r\n");    /* 存活标记 */

    for (;;) {
        while ((n = bsp_uart_read(&s_u_imu, buf, (int)sizeof(buf))) > 0) {
            bsp_imu_feed(buf, (uint16_t)n);
        }
        while ((n = bsp_uart_read(&s_u_vis, buf, (int)sizeof(buf))) > 0) {
            bsp_vision_feed(buf, (uint16_t)n);
        }
        while ((n = bsp_uart_read(&s_u_bt, buf, (int)sizeof(buf))) > 0) {
            int k;
            bt_feed(buf, (uint16_t)n);
            /* 蓝牙同时当命令行用：只挑可打印 ASCII，二进制帧里的 0xAA 之类会被滤掉，
             * 这样同一个 HC06 既能发命令、也不会被遥测帧污染。 */
            for (k = 0; k < n; k++) {
                uint8_t c = buf[k];
                if ((c >= 0x20u && c <= 0x7Eu) || c == '\r' || c == '\n') {
                    cli_feed(c);
                }
            }
            cli_poll();
        }

        /* 调试串口：台架命令行（也接受从蓝牙打进来的同一套命令） */
        {
            int k;
            while ((n = bsp_uart_read(&s_u_dbg, buf, (int)sizeof(buf))) > 0) {
                for (k = 0; k < n; k++) cli_feed(buf[k]);
            }
            cli_poll();     /* 没有回车也能执行 */
        }

        /* 串口心跳：没收到过任何命令时，每 500 ms 打一个点。
         * 用于区分"任务没跑"和"接收线路不通"：
         *   有点   → comm_task 在跑，TX 正常，问题在 RX（线序/换行）
         *   没点   → comm_task 没跑起来（卡死或没创建）
         * 收到第一条命令后自动静默。 */
        if (!s_cli_seen && (uint32_t)(HAL_GetTick() - next_beat) >= 500u) {
            next_beat = HAL_GetTick();
            app_log(".");
        }

        /* w 打开时的连续监控：每 200 ms 报一次编码器和轮速 */
        if (s_watch && (uint32_t)(HAL_GetTick() - next_watch) >= 200u) {
            car_state_t ws;
            next_watch = HAL_GetTick();
            if (st_snap(ctrl_shadow(), &ws)) {
                app_log("[enc] L=%d R=%d  vL=%d vR=%d  RA=%u RB=%u LA=%u LB=%u\r\n",
                        (int)bsp_encoder_raw_l(), (int)bsp_encoder_raw_r(),
                        (int)ws.speed_l, (int)ws.speed_r,
                        (unsigned)((GPIOA->IDR >> 6) & 1u),   /* ENC_R_A = PA6 */
                        (unsigned)((GPIOA->IDR >> 7) & 1u),   /* ENC_R_B = PA7 */
                        (unsigned)((GPIOB->IDR >> 6) & 1u),   /* ENC_L_A = PB6 */
                        (unsigned)((GPIOB->IDR >> 7) & 1u));  /* ENC_L_B = PB7 */
            }
        }

        /* d 打开时的连续监控：每 100 ms 一行 8 位二进制。
         * 手拿模块在赛道上来回扫，一眼就能看出哪几路在响应。 */
        if (s_trk_watch && (uint32_t)(HAL_GetTick() - next_trkw) >= 100u) {
            car_state_t ds;
            next_trkw = HAL_GetTick();
            if (st_snap(ctrl_shadow(), &ds)) {
                char hit[32];
                name_hit(hit, ds.track_mask);
                app_log("%s e=%d\r\n", hit, (int)(ds.line_err * 100.0f));
            }
        }

        /* o 打开时的速度曲线：每 50 ms 一行 "<目标> <左> <右>" */
        if (s_speed_trk && (uint32_t)(HAL_GetTick() - next_spd) >= 50u) {
            car_state_t ss;
            next_spd = HAL_GetTick();
            if (st_snap(ctrl_shadow(), &ss)) {
                app_log("[o] %d %d %d %d %d\r\n",
                        (int)ss.v_ref_l, (int)ss.speed_l, (int)ss.speed_r,
                        (int)ss.duty_l, (int)ss.duty_r);
            }
        }

        /* 蓝牙状态改由 v 命令按需查询，不再自动打印，避免占用链路 */
        (void)next_btdbg;

        /* 遥测默认关闭（会和日志抢同一个蓝牙口）。命令 y 可开关。 */
        if (s_telem_on && (uint32_t)(HAL_GetTick() - next_telem) >= CFG_TELEM_PERIOD_MS) {
            next_telem = HAL_GetTick() + CFG_TELEM_PERIOD_MS;
            send_telemetry();
        }
        if ((uint32_t)(HAL_GetTick() - next_health) >= 100u) {
            next_health = HAL_GetTick() + 100u;
            bsp_uart_check(&s_u_imu);
            bsp_uart_check(&s_u_vis);
            bsp_uart_check(&s_u_bt);
            bsp_uart_check(&s_u_dbg);
        }

        vTaskDelay(pdMS_TO_TICKS(CFG_COMM_POLL_MS));
    }
}

static void ui_task(void *arg)
{
    uint32_t beat = 0u;
    (void)arg;

    app_log("\r\n[ui alive]\r\n");      /* 存活标记 */

    bsp_ui_oled_init();

    for (;;) {
        car_state_t st;
        uint8_t i, ok = 0u;

        for (i = 0; i < 3u; i++) {
            if (st_snap(ctrl_shadow(), &st)) { ok = 1u; break; }
        }

        /* LED1 = 心跳。活着就闪（5 × 100 ms 翻转一次 ≈ 1 Hz）。
         * 这是没有串口时唯一能确认"任务还在跑"的手段：
         *   闪  → 调度器在跑，问题在串口接收或线序
         *   不闪 → 系统卡死/任务没建起来，别看串口了 */
        beat++;
        bsp_ui_led(0u, (beat % 10u) < 5u);

        if (ok) {
            /* LED2 只报"关键故障"。视觉丢失/丢线在台架上必然存在
             * （没接 K230、没接循迹），不该让灯一直亮着吓人。 */
            bsp_ui_led(1u, (st.fault & (FAULT_ESTOP | FAULT_LOW_BATT | FAULT_STALL)) != 0u);
            bsp_ui_buzzer((st.fault & FAULT_LOW_BATT) != 0u);
        }

        vTaskDelay(pdMS_TO_TICKS(CFG_UI_PERIOD_MS));
    }
}

/* ================= 初始化 / 启动 ================= */

void app_init(void)
{
    /* 1. 先把所有输出拉到安全态 */
    bsp_motor_init();
    bsp_ui_init();

    /* 2. 输入设备 */
    bsp_encoder_init();
    bsp_track_init();
    bsp_imu_init();
    bsp_vision_init();
    proto_rx_reset(&s_bt_parser);

    /* 3. 串口（DMA 已由 CubeMX 初始化） */
    bsp_uart_start(&s_u_dbg, &huart1);
    bsp_uart_start(&s_u_imu, &huart2);
    bsp_uart_start(&s_u_bt,  &huart3);
    bsp_uart_start(&s_u_vis, &huart6);

    /* 3.5 循迹 I2C 自检：上电就打一行，确认 0x12 有没有应答。
     * probe=1 说明从机 ACK 了（总线通、地址对、供电正常）。
     * probe=0 说明：模块没供电 / 线接错 / SCL-SDA 反了 / 模块坏了。 */
    app_log("[track] probe 0x%02X = %u (swapSCL/SDA=%u)\r\n",
            (unsigned)CFG_TRACK_ADDR,
            (unsigned)bsp_track_probe(CFG_TRACK_ADDR),
            (unsigned)CFG_TRACK_SWAP_SCL_SDA);

    /* 4. 控制层 */
    ctrl_init();

    /* 5. 等 IMU 稳定（最多 500 ms），失败也继续跑。
     *    注意：这里还没启动调度器，只能用 HAL_Delay，绝对不能用 vTaskDelay！ */
    {
        uint32_t t0 = HAL_GetTick();
        imu_t imu;
        while ((uint32_t)(HAL_GetTick() - t0) < 500u) {
            uint8_t b[32];
            int n;
            while ((n = bsp_uart_read(&s_u_imu, b, (int)sizeof(b))) > 0) {
                bsp_imu_feed(b, (uint16_t)n);
            }
            if (bsp_imu_get(&imu) && (uint32_t)(HAL_GetTick() - imu.last_ms) < 100u) break;
            HAL_Delay(5u);
        }
    }

    /* 6. 电机保持断电，等进入 TRACK/REMOTE 模式时由 ctrl 使能。
     *    上电后电机绝不转，这是硬规矩。 */
    HAL_Delay(100u);

    app_log("\r\n[car] boot ok, sysclk=%lu\r\n", (unsigned long)SystemCoreClock);
}

void app_start(void)
{
    BaseType_t r1, r2, r3;

    /* 建任务前先报一次剩余堆。如果这里数字很小，说明堆不够，后面会静默失败 */
    app_log("[app] heap free = %d\r\n", (int)xPortGetFreeHeapSize());

    /* 栈给足：AC5 + 大量浮点局部变量，1 KB 很紧。溢出会进死循环钩子，现象就是"安静地卡住" */
    r1 = xTaskCreate(ctrl_task, "ctrl", 512u, 0, 5u, &s_task_ctrl);
    r2 = xTaskCreate(comm_task, "comm", 512u, 0, 3u, 0);
    r3 = xTaskCreate(ui_task,   "ui",   384u, 0, 1u, 0);

    /* pdPASS = 1。任何一个不是 1，就是堆不够 */
    app_log("[app] create ctrl=%d comm=%d ui=%d heap=%d\r\n",
            (int)r1, (int)r2, (int)r3, (int)xPortGetFreeHeapSize());
}

/* ================= TIM6 节拍中断 ================= */

void app_tim6_isr(void)
{
    BaseType_t woken = pdFALSE;

    if (s_task_ctrl != 0) {
        vTaskNotifyGiveFromISR(s_task_ctrl, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/*
 * 【注意】这里故意不再定义 HAL_TIM_PeriodElapsedCallback。
 *
 * CubeMX 已经在 Core/Src/main.c 里生成了一个，负责 TIM7 -> HAL_IncTick()。
 * 那个函数在 USER CODE 之外，重新生成会重写，所以正确做法是：
 *   在 main.c 的 "USER CODE BEGIN Callback 0" 段里加一行：
 *       else if (htim->Instance == TIM6) { app_tim6_isr(); }
 *
 * 这样：
 *   - 不会出现 multiply defined 链接错误；
 *   - CubeMX 重新生成也不会冲掉我们的 TIM6 钩子；
 *   - HAL 时基（TIM7）的控制权仍归 CubeMX。
 *
 * TIM6 的中断服务走的是 HAL_TIM_IRQHandler -> HAL_TIM_PeriodElapsedCallback。
 */

/**
 * @file  bsp_track.c
 *
 * 【CubeMX 引脚标签】TRACK_SCL = PD3，TRACK_SDA = PD4
 *   必须配置为 GPIO_MODE_OUTPUT_OD（开漏），外部 4.7k 上拉到 5V。
 *   PD3/PD4 在 F407 上是 FT 引脚，5V 上拉安全 —— 但绝不能配成推挽。
 *
 * 为什么用寄存器直写 BSRR 而不是 HAL_GPIO_WritePin：
 *   HAL 写一个引脚约 20~30 周期，400 kHz 下每个半周期只有 200 周期预算，
 *   直接写 BSRR 只要 2 个周期，时序才稳得住。
 */
#include "bsp_track.h"
#include "app_config.h"
#include "main.h"

static uint32_t s_half_ticks;
static uint8_t  s_inited;

/* ------------------------------------------------------------------
 * SCL / SDA 角色分配（运行时可切换）
 *
 * 软件 I2C 的优势：哪根线当时钟、哪根线当数据完全由代码决定，
 * PCB 画反了也能救回来，不用飞线。电气上两条线都是开漏 + 上拉 + FT，对调无害。
 *
 * 用运行时变量而不是编译期宏，是为了能用命令 `s` 现场切换 + 自动探测，
 * 让模块自己回答"哪种接法是对的"，而不是靠人回忆原理图。
 * ------------------------------------------------------------------ */
static GPIO_TypeDef *s_scl_port;
static GPIO_TypeDef *s_sda_port;
static uint16_t      s_scl_pin;
static uint16_t      s_sda_pin;
static uint8_t       s_swap;

static void apply_pins(void)
{
    if (s_swap) {
        s_scl_port = TRACK_SDA_GPIO_Port;  s_scl_pin = TRACK_SDA_Pin;
        s_sda_port = TRACK_SCL_GPIO_Port;  s_sda_pin = TRACK_SCL_Pin;
    } else {
        s_scl_port = TRACK_SCL_GPIO_Port;  s_scl_pin = TRACK_SCL_Pin;
        s_sda_port = TRACK_SDA_GPIO_Port;  s_sda_pin = TRACK_SDA_Pin;
    }
}

/* ---------------- 底层时序 ---------------- */

static inline void dly(void)
{
    uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < s_half_ticks) { }
}

/* 释放 SCL 后要**等它真的变高**——从机会做时钟延展（clock stretching），
 * 把 SCL 拉低「喘口气」。不等就发下一位，时序就全乱了。
 * 典型症状：地址字节 ACK、紧跟着的寄存器字节 NACK（rc=-3）。
 *
 * ⚠️ 超时必须**很短**：这个函数在 ctrl 任务里每 5 ms 被调几十次，
 * 一旦总线被拉死，超时太长会把 CPU 吃光、低优先级任务全部饿死
 * （现象就是"命令行完全没反应"）。200 次迭代 ≈ 1.2 µs，够时钟延展用。 */
static inline void scl_hi(void)
{
    uint32_t guard = 0u;
    s_scl_port->BSRR = (uint32_t)s_scl_pin;
    while (((s_scl_port->IDR & s_scl_pin) == 0u) && (++guard < 200u)) { }
}

static inline void scl_lo(void) { s_scl_port->BSRR = (uint32_t)s_scl_pin << 16; }
static inline void sda_hi(void) { s_sda_port->BSRR = (uint32_t)s_sda_pin; }
static inline void sda_lo(void) { s_sda_port->BSRR = (uint32_t)s_sda_pin << 16; }
static inline uint8_t sda_rd(void)
{
    return (s_sda_port->IDR & s_sda_pin) ? 1u : 0u;
}

static void i2c_start(void)
{
    sda_hi(); dly();
    scl_hi(); dly();
    sda_lo(); dly();
    scl_lo(); dly();
}

static void i2c_stop(void)
{
    sda_lo(); dly();
    scl_hi(); dly();
    sda_hi(); dly();
}

static void i2c_write_bit(uint8_t b)
{
    if (b) sda_hi(); else sda_lo();
    dly();
    scl_hi(); dly();
    scl_lo(); dly();
}

static uint8_t i2c_read_bit(void)
{
    uint8_t b;
    sda_hi();
    dly();
    scl_hi(); dly();
    b = sda_rd();
    scl_lo(); dly();
    return b;
}

/* 返回 1 = 收到 ACK */
static uint8_t i2c_write_byte(uint8_t v)
{
    uint8_t i, nack;
    for (i = 0; i < 8u; i++) i2c_write_bit((uint8_t)((v >> (7u - i)) & 1u));
    nack = i2c_read_bit();          /* 0 = ACK */
    return (uint8_t)(nack == 0u);
}

static uint8_t i2c_read_byte(uint8_t ack)
{
    uint8_t i, v = 0u;
    for (i = 0; i < 8u; i++) v = (uint8_t)((v << 1) | i2c_read_bit());
    i2c_write_bit(ack ? 0u : 1u);   /* 主机给 ACK = 拉低 */
    return v;
}

/* 直接读一个字节：START → 地址(读) → 收 1 字节 → STOP。
 * 不写寄存器地址，因此**没有重复起始**。
 * 依据：f 命令扫描 0x00~0x3F 全部返回同一个字节 → 这个模块不分寄存器，
 * 而"傻瓜"模块常不支持重复起始，会导致数据错位。 */
static int i2c_read_direct(uint8_t addr, uint8_t *out)
{
    i2c_start();
    if (!i2c_write_byte((uint8_t)((addr << 1) | 1u))) { i2c_stop(); return -2; }
    *out = i2c_read_byte(0u);
    i2c_stop();
    return 0;
}


/* 传统寄存器读（带重复起始）。保留作对比 / 其他模块用。 */
static int i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t *out)
{
    i2c_start();
    if (!i2c_write_byte((uint8_t)(addr << 1))) { i2c_stop(); return -2; }
    if (!i2c_write_byte(reg))                  { i2c_stop(); return -3; }
    i2c_start();
    if (!i2c_write_byte((uint8_t)((addr << 1) | 1u))) { i2c_stop(); return -4; }
    *out = i2c_read_byte(0u);       /* 只读 1 字节 → NACK 收尾 */
    i2c_stop();
    return 0;
}

/* ---------------- 公共接口 ---------------- */

void bsp_track_init(void)
{
    GPIO_InitTypeDef g;
    float f;

    /* 开 DWT 周期计数器（只用于延时，不占外设） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    f = CFG_TRACK_I2C_HALF_US * ((float)SystemCoreClock / 1000000.0f);
    s_half_ticks = (f < 1.0f) ? 1u : (uint32_t)f;

    s_swap = (uint8_t)CFG_TRACK_SWAP_SCL_SDA;
    apply_pins();

    g.Pin   = TRACK_SCL_Pin | TRACK_SDA_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(TRACK_SCL_GPIO_Port, &g);

    scl_hi();
    sda_hi();
    s_inited = 1u;

    /* 清一下总线：如果上一次运行把从机搞成"拉住 SDA"的状态，
     * 上电后第一次事务会一直失败。打 9 个时钟 + STOP 就能让从机复位。 */
    bsp_track_recover();
}

/* 运行时切换 SCL/SDA 角色（现场排查用） */
void bsp_track_set_swap(uint8_t swap)
{
    s_swap = (uint8_t)(swap ? 1u : 0u);
    apply_pins();
    if (s_inited) {
        scl_hi();
        sda_hi();       /* 释放总线 */
    }
}

uint8_t bsp_track_get_swap(void)
{
    return s_swap;
}

/* 运行时改变 I2C 半周期（µs）。用于排查"读取偏移"是否与速度有关：
 * 慢到 100 kHz 以下如果偏移消失，说明是时序问题；否则是模块本身。 */
void bsp_track_set_half_us(uint32_t us)
{
    float f;
    if (us < 1u) us = 1u;
    f = (float)us * ((float)SystemCoreClock / 1000000.0f);
    s_half_ticks = (f < 1.0f) ? 1u : (uint32_t)f;
}

uint32_t bsp_track_half_us(void)
{
    return (uint32_t)((float)s_half_ticks * 1000000.0f / (float)SystemCoreClock + 0.5f);
}

/* 总线恢复：SDA 被从机拉死时，打 9 个时钟再发 STOP */
void bsp_track_recover(void)
{
    uint8_t i;
    sda_hi();
    for (i = 0; i < 9u; i++) {
        scl_lo(); dly();
        scl_hi(); dly();
    }
    i2c_stop();
}

uint8_t bsp_track_probe(uint8_t addr){
    uint8_t ack;
    if (!s_inited) return 0u;
    i2c_start();
    ack = i2c_write_byte((uint8_t)(addr << 1));
    i2c_stop();
    return ack;
}

/* 读任意寄存器：用于扫出真正的数据寄存器（模块的字节布局不一定按常规） */
int bsp_track_read_reg(uint8_t reg, uint8_t *out)
{
    if (!s_inited || out == 0) return -1;
    return i2c_read_reg8(CFG_TRACK_ADDR, reg, out);
}

/* 读一个字节（未归一化）。用于区分"读失败"和"读到了 0x00"。 */
int bsp_track_read_byte(uint8_t *out)
{
    if (!s_inited || out == 0) return -1;
#if CFG_TRACK_READ_MODE
    return i2c_read_direct(CFG_TRACK_ADDR, out);
#else
    return i2c_read_reg8(CFG_TRACK_ADDR, CFG_TRACK_REG, out);
#endif
}

/* 位序翻转：把 bit7..bit0 镜像成 bit0..bit7 */
static uint8_t reverse8(uint8_t x)
{
    x = (uint8_t)(((x & 0xF0u) >> 4) | ((x & 0x0Fu) << 4));
    x = (uint8_t)(((x & 0xCCu) >> 2) | ((x & 0x33u) << 2));
    x = (uint8_t)(((x & 0xAAu) >> 1) | ((x & 0x55u) << 1));
    return x;
}

int bsp_track_read(track_feat_t *f)
{
    static const float w[8] = {
        CFG_TRACK_W0, CFG_TRACK_W1, CFG_TRACK_W2, CFG_TRACK_W3,
        CFG_TRACK_W4, CFG_TRACK_W5, CFG_TRACK_W6, CFG_TRACK_W7
    };
    uint8_t raw = 0u, m, i, cnt = 0u;
    float sum = 0.0f;
    int rc;

    if (!f) return -1;
    if (!s_inited) return -1;

    rc = bsp_track_read_byte(&raw);
    if (rc != 0) {
        f->raw_phys = 0u;
        f->valid = 0u;
        f->junction = 0u;
        f->raw = 0u;
        f->count = 0u;
        f->err = 0.0f;
        f->left_edge = -1;
        f->right_edge = -1;
        return rc;
    }

    /* 注意：不要在这里做"变化时再读一次确认"的滤波。
     * 早期为了压住 7F/E0 跳变加过，但那个跳变的真因是 I2C 太快；
     * 加上滤波后一旦读取不稳，会**永久卡在旧值**，反而更难排查。 */

    /* 显示用：保留模块**原样返回**的字节（补偿之前），方便排查读取时序 */
    f->raw_phys = raw;

    /* 【可选补偿】如果读取方式没法修好，模块字节相对手册右移了 3 位
     * （L1→bit4 … L5→bit0，最低 3 位被填 1），可以打开这个补偿左移回来。
     * 但这会永久丢掉最低 3 路，所以优先修读取方式（CFG_TRACK_READ_MODE）。 */
#if CFG_TRACK_SHIFT3
    raw = (uint8_t)((raw << 3) | 0x07u);
#endif

    /* 归一化：统一成"1 = 压黑线" */
    m = (CFG_TRACK_BLACK_LEVEL == 1u) ? (uint8_t)(~raw) : raw;

    /* 位序对齐：把 X1(bit7) 翻到 bit0（最左），权重表才能直接用 */
    if (CFG_TRACK_ORDER_REVERSE) m = reverse8(m);

    f->raw = m;
    f->left_edge = -1;
    f->right_edge = -1;

    for (i = 0; i < 8u; i++) {
        if ((m >> i) & 1u) {
            cnt++;
            sum += w[i];
            if (f->left_edge < 0) f->left_edge = (int8_t)i;
            f->right_edge = (int8_t)i;
        }
    }

    f->count = cnt;
    f->valid = (uint8_t)(cnt > 0u);
    f->junction = (uint8_t)(cnt >= 8u);
    f->err = (cnt > 0u) ? (sum / (float)cnt) : 0.0f;

    return 0;
}

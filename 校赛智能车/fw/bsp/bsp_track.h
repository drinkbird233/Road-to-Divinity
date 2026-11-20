/**
 * @file  bsp_track.h
 * @brief 八路循迹（I2C 0x12，寄存器 0x30），PD3/PD4 软件 I2C。
 *
 * 输出已归一化：bit = 1 表示"压到黑线"，与模块实际电平无关。
 * 全白 → valid=0（丢线）；全黑 → junction=1（十字/停车线/黑斑，必须由状态机裁决）。
 */
#ifndef BSP_TRACK_H
#define BSP_TRACK_H

#include <stdint.h>

typedef struct {
    uint8_t raw_phys;   /* 从模块读回的原始字节（未归一化） */
    uint8_t raw;        /* 归一化后的 8 位，bit0 = x1 */
    uint8_t count;      /* 有效路数 */
    float   err;        /* 加权误差，单位 = 一个传感器间距，向左为负 */
    uint8_t valid;      /* 0 = 丢线 */
    uint8_t junction;   /* 1 = 全黑 */
    int8_t  left_edge;  /* 最左有效通道（-1 = 无） */
    int8_t  right_edge;
} track_feat_t;

void    bsp_track_init(void);
void    bsp_track_recover(void);

/* 读一次并解码。返回 0 = 成功；<0 = I2C 错误 */
int     bsp_track_read(track_feat_t *f);

/* 地址探测，1 = 有应答（自检用） */
uint8_t bsp_track_probe(uint8_t addr);

/* 只读原始字节（不归一化、不翻位序）。返回 0 = I2C 成功 */
int     bsp_track_read_byte(uint8_t *out);

/* 读任意寄存器，用于扫描模块的真实数据寄存器 */
int     bsp_track_read_reg(uint8_t reg, uint8_t *out);

/* 运行时切换 SCL/SDA 角色（PCB 画反时的补救，现场排查用） */
void    bsp_track_set_swap(uint8_t swap);
uint8_t bsp_track_get_swap(void);

/* 运行时改变 I2C 半周期（µs），用于排查读取偏移与时序的关系 */
void     bsp_track_set_half_us(uint32_t us);
uint32_t bsp_track_half_us(void);

#endif /* BSP_TRACK_H */

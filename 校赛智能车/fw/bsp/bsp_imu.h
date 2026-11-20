/**
 * @file  bsp_imu.h
 * @brief HWT101（维特智能）UART 解析。帧头 0x55，11 字节，第 11 字节为前 10 字节和校验。
 *
 * 支持 0x52（角速度）与 0x53（角度）两种帧，按需解析。
 * 【必须做的一件事】把模块波特率从 9600 改到 115200：
 *   11 B × 100 Hz × 10 bit = 11000 bit/s > 9600，9600 下最高只有 ~87 Hz。
 */
#ifndef BSP_IMU_H
#define BSP_IMU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float    yaw_deg;     /* -180..180 */
    float    gyro_dps;    /* z 轴角速度 */
    int16_t  yaw_ddeg;    /* 0.1° */
    int16_t  gyro_ddps;   /* 0.1°/s */
    uint32_t last_ms;
    uint32_t frames;
    uint32_t crc_err;
    uint32_t n52;         /* 收到多少个 0x52 角速度帧 */
    uint32_t n53;         /* 收到多少个 0x53 角度帧（=0 说明模块没输出角度） */
    uint32_t nother;      /* 其他类型的合法帧（0x51/0x54 等） */
    uint8_t  ok;
} imu_t;

void bsp_imu_init(void);
void bsp_imu_feed(const uint8_t *d, uint16_t n);

/* 副本读出；返回 false 表示从未收到过有效帧 */
bool bsp_imu_get(imu_t *out);

/* 把当前 yaw 作为新的零位（软件偏置，不是模块标定） */
void bsp_imu_zero(void);

#endif /* BSP_IMU_H */

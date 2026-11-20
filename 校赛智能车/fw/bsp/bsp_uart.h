/**
 * @file  bsp_uart.h
 * @brief 串口：DMA 环形接收 + 非阻塞发送。
 *
 * 【CubeMX 要求】RX 的 DMA Stream 必须设为 Circular，否则计数器会停住。
 * 收包不依赖 IDLE 中断，靠读 NDTR 差值取数，抗丢中断、抗溢出。
 */
#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

#define BSP_UART_BUF_SZ   256u

typedef struct {
    UART_HandleTypeDef *h;
    uint8_t   buf[BSP_UART_BUF_SZ];
    uint16_t  rpos;
    uint32_t  last_rx_tick;
    uint32_t  rx_total;     /* 累计收到的字节数（判断"到底有没有数据进来"） */
    uint8_t   started;
} bsp_uart_t;

void     bsp_uart_start(bsp_uart_t *u, UART_HandleTypeDef *h);

/* 取走新到达的字节，返回个数（0 表示没新数据） */
int      bsp_uart_read(bsp_uart_t *u, uint8_t *out, int max);

/* DMA 发送；上一帧还没发完则直接返回 false（不阻塞、不排队） */
bool     bsp_uart_send(bsp_uart_t *u, const uint8_t *d, uint16_t n);

/* 阻塞发送，仅用于 printf / 初始化阶段 */
bool     bsp_uart_send_blocking(bsp_uart_t *u, const uint8_t *d, uint16_t n);

/* 看门狗：DMA 掉了就重启，建议每 100 ms 调一次 */
void     bsp_uart_check(bsp_uart_t *u);

uint32_t bsp_uart_last_rx(const bsp_uart_t *u);

/* 累计收到的字节总数 */
uint32_t bsp_uart_rx_total(const bsp_uart_t *u);

#endif /* BSP_UART_H */

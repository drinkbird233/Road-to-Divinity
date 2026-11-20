/**
 * @file  bsp_uart.c
 */
#include "bsp_uart.h"

static uint16_t rx_pos(const bsp_uart_t *u)
{
    uint16_t ndtr;
    if (u->h == 0 || u->h->hdmarx == 0) return 0u;
    ndtr = (uint16_t)u->h->hdmarx->Instance->NDTR;
    if (ndtr > BSP_UART_BUF_SZ) ndtr = BSP_UART_BUF_SZ;
    return (uint16_t)(BSP_UART_BUF_SZ - ndtr);
}

void bsp_uart_start(bsp_uart_t *u, UART_HandleTypeDef *h)
{
    u->h = h;
    u->rpos = 0u;
    u->last_rx_tick = HAL_GetTick();
    u->rx_total = 0u;
    u->started = 0u;

    if (h->hdmarx != 0) {
        if (HAL_UART_Receive_DMA(h, u->buf, BSP_UART_BUF_SZ) == HAL_OK) {
            u->started = 1u;
        }
    }
}

int bsp_uart_read(bsp_uart_t *u, uint8_t *out, int max)
{
    uint16_t pos;
    int n = 0;

    if (!u->started || out == 0 || max <= 0) return 0;

    pos = rx_pos(u);
    while (u->rpos != pos && n < max) {
        out[n++] = u->buf[u->rpos];
        u->rpos = (uint16_t)((u->rpos + 1u) % BSP_UART_BUF_SZ);
    }
    if (n > 0) {
        u->last_rx_tick = HAL_GetTick();
        u->rx_total += (uint32_t)n;
    }
    return n;
}

bool bsp_uart_send(bsp_uart_t *u, const uint8_t *d, uint16_t n)
{
    if (u->h == 0 || d == 0 || n == 0) return false;
    if (u->h->gState != HAL_UART_STATE_READY) return false;
    return (HAL_UART_Transmit_DMA(u->h, (uint8_t *)d, n) == HAL_OK);
}

bool bsp_uart_send_blocking(bsp_uart_t *u, const uint8_t *d, uint16_t n)
{
    uint32_t t0;

    if (u->h == 0 || d == 0 || n == 0) return false;

    /* 直接调 HAL_UART_Transmit 时，如果 UART 正忙（比如 DMA 正在发遥测帧），
     * 它会**立刻返回 HAL_BUSY**，而调用方通常不看返回值 —— 整行日志就凭空消失了。
     * 这里先等它空闲（最多 20 ms），再发。 */
    t0 = HAL_GetTick();
    while (u->h->gState != HAL_UART_STATE_READY) {
        if ((uint32_t)(HAL_GetTick() - t0) > 20u) break;
    }

    return (HAL_UART_Transmit(u->h, (uint8_t *)d, n, 50u) == HAL_OK);
}

void bsp_uart_check(bsp_uart_t *u)
{
    if (u->h == 0 || u->h->hdmarx == 0) return;

    /* DMA 被错误中断打死 / 溢出 / 初始化失败时重启 */
    if (u->h->RxState != HAL_UART_STATE_BUSY_RX) {
        __HAL_UART_CLEAR_OREFLAG(u->h);
        u->rpos = 0u;
        u->started = (HAL_UART_Receive_DMA(u->h, u->buf, BSP_UART_BUF_SZ) == HAL_OK) ? 1u : 0u;
    }
}

uint32_t bsp_uart_last_rx(const bsp_uart_t *u)
{
    return u->last_rx_tick;
}

uint32_t bsp_uart_rx_total(const bsp_uart_t *u)
{
    return u->rx_total;
}

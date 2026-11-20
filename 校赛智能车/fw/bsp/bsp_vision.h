/**
 * @file  bsp_vision.h
 * @brief K230 视觉帧解析（TYPE = 0x10，8 字节 payload）。
 *
 * 视觉是"事件源/前馈源"，不是控制回路的一部分：延迟 60–100 ms，绝不能进反馈。
 */
#ifndef BSP_VISION_H
#define BSP_VISION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t  x;          /* -1000..1000，向右为正 */
    int16_t  y;
    uint8_t  label;
    uint8_t  valid;      /* 1 = 本帧检测到目标 */
    uint16_t frame_id;
    uint32_t last_ms;
    uint32_t frames;
    uint32_t crc_err;
    uint16_t drop;       /* frame_id 丢帧计数 */
} vision_t;

void bsp_vision_init(void);
void bsp_vision_feed(const uint8_t *d, uint16_t n);
bool bsp_vision_get(vision_t *out);
void bsp_vision_note_error(void);

#endif /* BSP_VISION_H */

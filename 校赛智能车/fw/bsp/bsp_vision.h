/**
 * @file  bsp_vision.h
 * @brief K230 视觉帧解析（TYPE = 0x10）。
 *
 * 视觉是"事件/前馈源"，不是控制回路的一部分：延迟 60–100 ms，绝不能进反馈。
 * 它相对 8 路灰度的核心价值：**提前几十厘米**知道前方是直角弯、断口还是环岛。
 * 8 路灰度视野只有约 70 mm，车到了弯口才知道要转弯，那时已经晚了。
 */
#ifndef BSP_VISION_H
#define BSP_VISION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t  x;          /* -1000..1000，向右为正（线的横向偏差） */
    int16_t  y;          /* 前瞻距离 mm（可选） */
    int16_t  angle;      /* 0.1°，线相对车头的方向，右转为正 */
    uint8_t  flags;      /* 见 VIS_FLAG_* */
    uint8_t  quality;    /* 0..100 置信度 */
    uint8_t  label;
    uint8_t  valid;      /* 1 = 本帧检测到目标 */
    uint16_t frame_id;
    uint32_t last_ms;
    uint32_t frames;
    uint32_t crc_err;
    uint16_t drop;       /* frame_id 丢帧计数 */
} vision_t;

/* flags 的 bit0..2：前方元素类型 */
#define VIS_FLAG_NONE       0u   /* 纯直线 */
#define VIS_FLAG_LEFT90     1u   /* 前方左直角 */
#define VIS_FLAG_RIGHT90    2u   /* 前方右直角 */
#define VIS_FLAG_GAP        3u   /* 前方断线口（盲走区） */
#define VIS_FLAG_LOOP       4u   /* 前方环岛入口 */
#define VIS_FLAG_KIND_MASK  0x07u
#define VIS_FLAG_HAS_LINE   0x08u/* 当前帧看到线 */

const char *vis_flag_str(uint8_t flags);

void bsp_vision_init(void);
void bsp_vision_feed(const uint8_t *d, uint16_t n);
bool bsp_vision_get(vision_t *out);
void bsp_vision_note_error(void);

#endif /* BSP_VISION_H */

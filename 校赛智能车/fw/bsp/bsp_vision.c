/**
 * @file  bsp_vision.c
 *
 * payload 布局（小端，长度 8 或 12 都接受，向后兼容）：
 *   [0]    valid      1 = 本帧有效
 *   [1..2] target_x   线横向偏差 -1000..1000（右正）
 *   [3..4] target_y   前瞻距离 mm（可选）
 *   [5..6] frame_id   帧号，用来统计丢帧
 *   [7]    label      标签
 *   [8..9] angle      0.1°，线的方向（右正）—— 只有 >=10 字节时才有
 *   [10]   flags      前方元素类型（见 VIS_FLAG_*）—— 只有 >=11 字节时才有
 *   [11]   quality    0..100 置信度
 */
#include "bsp_vision.h"
#include "proto.h"
#include "app_types.h"
#include "main.h"

static proto_rx_t       s_parser;
static vision_t         s_v;
static volatile uint32_t s_seq;

static void on_frame(uint8_t type, const uint8_t *pl, uint8_t len, void *user)
{
    uint16_t fid;
    (void)user;

    if (type != PROTO_T_VISION || len < 8u) return;

    fid = get_u16(&pl[5]);

    s_seq++;
    APP_BARRIER();

    /* 丢帧统计：只在"帧号正常前进一点"时累加。
     * 原来没做范围判断，帧号一旦乱序（或上电时 K230 已经在跑很远）
     * 就会一次性加上 65534，看起来像丢了六万帧，其实是假的。 */
    if (s_v.frames > 0u) {
        uint16_t d = (uint16_t)(fid - s_v.frame_id);
        if (d > 1u && d < 100u) {
            s_v.drop = (uint16_t)(s_v.drop + (uint16_t)(d - 1u));
        }
    }

    s_v.valid    = pl[0];
    s_v.x        = get_i16(&pl[1]);
    s_v.y        = get_i16(&pl[3]);
    s_v.frame_id = fid;
    s_v.label    = pl[7];
    /* 8 字节是旧格式（只有位置），10 字节起有角度，11 字节起有 flags。
     * 向后兼容：K230 程序没升级时也不会解析出错。 */
    s_v.angle    = (len >= 10u) ? get_i16(&pl[8]) : 0;
    s_v.flags    = (len >= 11u) ? pl[10] : 0u;
    s_v.quality  = (len >= 12u) ? pl[11] : 100u;
    s_v.last_ms  = HAL_GetTick();
    s_v.frames++;

    APP_BARRIER();
    s_seq++;
}

void bsp_vision_init(void)
{
    proto_rx_reset(&s_parser);
    s_v.x = 0;
    s_v.y = 0;
    s_v.angle = 0;
    s_v.flags = 0u;
    s_v.quality = 0u;
    s_v.label = 0u;
    s_v.valid = 0u;
    s_v.frame_id = 0u;
    s_v.last_ms = 0u;
    s_v.frames = 0u;
    s_v.crc_err = 0u;
    s_v.drop = 0u;
}

void bsp_vision_note_error(void)
{
    s_v.crc_err++;
}

void bsp_vision_feed(const uint8_t *d, uint16_t n)
{
    uint16_t i;
    if (!d) return;
    for (i = 0; i < n; i++) proto_rx_feed(&s_parser, d[i], on_frame, 0);
}

bool bsp_vision_get(vision_t *out)
{
    uint32_t a, b;
    if (!out) return false;

    a = s_seq;
    if (a & 1u) return false;
    APP_BARRIER();
    *out = s_v;
    APP_BARRIER();
    b = s_seq;

    return (a == b) && (out->frames > 0u);
}

/* flags 转成可读字符串，日志和调参用 */
const char *vis_flag_str(uint8_t flags)
{
    switch (flags & VIS_FLAG_KIND_MASK) {
    case VIS_FLAG_LEFT90:  return "LEFT90";
    case VIS_FLAG_RIGHT90: return "RIGHT90";
    case VIS_FLAG_GAP:     return "GAP";
    case VIS_FLAG_LOOP:    return "LOOP";
    default:               return "LINE";
    }
}

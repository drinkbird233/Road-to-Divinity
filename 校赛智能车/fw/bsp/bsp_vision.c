/**
 * @file  bsp_vision.c
 *
 * payload 布局（小端）：
 *   [0] valid  [1..2] target_x  [3..4] target_y  [5..6] frame_id  [7] label
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

    if (s_v.frames > 0u && (uint16_t)(fid - s_v.frame_id) > 1u) {
        s_v.drop = (uint16_t)(s_v.drop + (uint16_t)(fid - s_v.frame_id - 1u));
    }

    s_v.valid    = pl[0];
    s_v.x        = get_i16(&pl[1]);
    s_v.y        = get_i16(&pl[3]);
    s_v.frame_id = fid;
    s_v.label    = pl[7];
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

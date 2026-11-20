/**
 * @file  proto.c
 */
#include "proto.h"

uint8_t crc8_update(uint8_t crc, uint8_t byte)
{
    uint8_t i;
    crc ^= byte;
    for (i = 0; i < 8u; i++) {
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint8_t crc8(const uint8_t *d, uint16_t n)
{
    uint8_t c = 0x00u;
    uint16_t i;
    for (i = 0; i < n; i++) c = crc8_update(c, d[i]);
    return c;
}

void proto_rx_reset(proto_rx_t *p)
{
    p->state = 0u;
    p->type  = 0u;
    p->len   = 0u;
    p->idx   = 0u;
    p->crc   = 0x00u;
}

void proto_rx_feed(proto_rx_t *p, uint8_t byte, proto_cb_t cb, void *user)
{
    switch (p->state) {
    case 0:
        if (byte == PROTO_HEAD0) { p->state = 1u; p->crc = 0x00u; }
        break;

    case 1:
        if (byte == PROTO_HEAD1) {
            p->state = 2u;
        } else {
            p->state = (byte == PROTO_HEAD0) ? 1u : 0u;
        }
        break;

    case 2:                                   /* TYPE */
        p->type = byte;
        p->crc  = crc8_update(0x00u, byte);
        p->state = 3u;
        break;

    case 3:                                   /* LEN */
        p->len   = byte;
        p->crc   = crc8_update(p->crc, byte);
        p->idx   = 0u;
        p->state = (byte <= PROTO_MAX_PAYLOAD && byte > 0u) ? 4u : 0u;
        break;

    case 4:                                   /* PAYLOAD */
        p->buf[p->idx++] = byte;
        p->crc = crc8_update(p->crc, byte);
        if (p->idx >= p->len) p->state = 5u;
        break;

    case 5:                                   /* CRC */
        if (byte == p->crc && cb) {
            cb(p->type, p->buf, p->len, user);
        }
        p->state = 0u;
        break;

    default:
        p->state = 0u;
        break;
    }
}

uint16_t proto_build(uint8_t *out, uint16_t cap, uint8_t type,
                     const uint8_t *payload, uint8_t len)
{
    uint16_t n;
    uint8_t c;

    if (len > PROTO_MAX_PAYLOAD) return 0u;
    if (cap < (uint16_t)(len + 5u)) return 0u;

    out[0] = PROTO_HEAD0;
    out[1] = PROTO_HEAD1;
    out[2] = type;
    out[3] = len;
    for (n = 0; n < len; n++) out[4 + n] = payload[n];

    c = crc8(&out[2], (uint16_t)(len + 2u));
    out[4u + len] = c;

    return (uint16_t)(len + 5u);
}

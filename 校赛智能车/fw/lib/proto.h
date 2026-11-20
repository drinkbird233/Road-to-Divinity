/**
 * @file  proto.h
 * @brief 统一帧协议：0xAA 0x55 | TYPE | LEN | PAYLOAD | CRC8
 *
 * CRC8: poly 0x07, init 0x00, 覆盖 TYPE + LEN + PAYLOAD；小端序。
 * 纯 C，不依赖 HAL。
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#define PROTO_HEAD0         0xAAu
#define PROTO_HEAD1         0x55u
#define PROTO_MAX_PAYLOAD   32u

/* 类型 */
#define PROTO_T_TELEMETRY   0x01u
#define PROTO_T_COMMAND     0x02u
#define PROTO_T_ACK         0x82u
#define PROTO_T_VISION      0x10u

/* 命令码 */
#define CMD_START           0x01u
#define CMD_STOP            0x02u
#define CMD_SET_SPEED       0x03u
#define CMD_SET_PID         0x04u
#define CMD_SET_MODE        0x05u
#define CMD_REQ_TELEMETRY   0x06u
#define CMD_CALIB_IMU       0x07u
#define CMD_SET_STREAM_RATE 0x08u

typedef void (*proto_cb_t)(uint8_t type, const uint8_t *payload, uint8_t len, void *user);

typedef struct {
    uint8_t state;
    uint8_t type;
    uint8_t len;
    uint8_t idx;
    uint8_t crc;
    uint8_t buf[PROTO_MAX_PAYLOAD];
} proto_rx_t;

void     proto_rx_reset(proto_rx_t *p);
void     proto_rx_feed(proto_rx_t *p, uint8_t byte, proto_cb_t cb, void *user);
uint8_t  crc8(const uint8_t *d, uint16_t n);
uint8_t  crc8_update(uint8_t crc, uint8_t byte);

/* 组帧，返回总长度；cap 不够返回 0 */
uint16_t proto_build(uint8_t *out, uint16_t cap, uint8_t type,
                     const uint8_t *payload, uint8_t len);

/* 小端读写工具 */
static inline void put_i16(uint8_t *p, int16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
static inline int16_t get_i16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static inline uint16_t get_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

#endif /* PROTO_H */

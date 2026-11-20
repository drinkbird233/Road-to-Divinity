/**
 * @file  bsp_imu.c
 */
#include "bsp_imu.h"
#include "app_config.h"
#include "app_types.h"
#include "main.h"

static uint8_t  s_buf[11];
static uint8_t  s_idx;
static float    s_yaw_zero;
static imu_t    s_imu;
static volatile uint32_t s_seq;

static float wrap180(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static void publish_begin(void) { s_seq++; APP_BARRIER(); }
static void publish_end(void)   { APP_BARRIER(); s_seq++; }

void bsp_imu_init(void)
{
    s_idx = 0u;
    s_yaw_zero = 0.0f;
    s_imu.yaw_deg = 0.0f;
    s_imu.gyro_dps = 0.0f;
    s_imu.yaw_ddeg = 0;
    s_imu.gyro_ddps = 0;
    s_imu.last_ms = 0u;
    s_imu.frames = 0u;
    s_imu.crc_err = 0u;
    s_imu.n52 = 0u;
    s_imu.n53 = 0u;
    s_imu.nother = 0u;
    s_imu.ok = 0u;
}

void bsp_imu_zero(void)
{
    s_yaw_zero = s_imu.yaw_deg;
}

void bsp_imu_feed(const uint8_t *d, uint16_t n)
{
    uint16_t i;
    uint8_t  recompute = 0u;

    if (!d) return;

    for (i = 0; i < n; i++) {
        uint8_t b = d[i];

        if (s_idx == 0u && b != 0x55u) continue;

        s_buf[s_idx++] = b;
        if (s_idx < 11u) continue;

        /* 整帧到手：校验前 10 字节之和 */
        {
            uint8_t sum = 0u, k;
            for (k = 0; k < 10u; k++) sum = (uint8_t)(sum + s_buf[k]);

            if (sum == s_buf[10]) {
                recompute = 1u;
            } else {
                s_imu.crc_err++;
                recompute = 0u;
            }
        }

        if (recompute) {
            uint8_t type = s_buf[1];

            /* WIT 0x53 角度帧布局：[2..3]=Roll  [4..5]=Pitch  [6..7]=Yaw
             * （之前误把 [4..5] 当 yaw，读到的是 Pitch，平放时恒为 ~0） */
            if (type == 0x53u) {
                int16_t yaw_raw = (int16_t)(((uint16_t)s_buf[7] << 8) | s_buf[6]);
                float yaw = (float)yaw_raw * CFG_IMU_ANGLE_LSB;
                publish_begin();
                s_imu.yaw_deg = wrap180(yaw - s_yaw_zero);
                s_imu.yaw_ddeg = (int16_t)(s_imu.yaw_deg * 10.0f);
                s_imu.last_ms = HAL_GetTick();
                s_imu.frames++;
                s_imu.n53++;
                s_imu.ok = 1u;
                publish_end();
            } else if (type == 0x52u) {              /* 角速度帧：[2..3]=wx [4..5]=wy [6..7]=wz */
                int16_t wz_raw = (int16_t)(((uint16_t)s_buf[7] << 8) | s_buf[6]);
                float wz = (float)wz_raw * CFG_IMU_GYRO_LSB;
                publish_begin();
                s_imu.gyro_dps = wz;
                s_imu.gyro_ddps = (int16_t)(wz * 10.0f);
                s_imu.last_ms = HAL_GetTick();
                s_imu.frames++;
                s_imu.n52++;
                s_imu.ok = 1u;
                publish_end();
            } else {
                s_imu.nother++;
            }
        }

        /* 重新同步：若帧尾本身是 0x55，它可能就是下一帧的帧头 */
        if (s_buf[10] == 0x55u) {
            s_buf[0] = 0x55u;
            s_idx = 1u;
        } else {
            s_idx = 0u;
        }
    }
}

bool bsp_imu_get(imu_t *out)
{
    uint32_t a, b;
    if (!out) return false;

    a = s_seq;
    if (a & 1u) return false;
    APP_BARRIER();
    *out = s_imu;
    APP_BARRIER();
    b = s_seq;

    return (a == b) && (out->frames > 0u);
}

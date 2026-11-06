#include "mp3_player.h"
#include "minimp3.h"
#include "fatfs.h"
#include <stddef.h>
#include <string.h>

#define AUDIO_HALF_BYTES      4608                      /* 一帧 PCM：1152 采样 × 2 声道 × 2 字节 */
#define AUDIO_HALF_HALFWORDS  (AUDIO_HALF_BYTES / 2)
#define AUDIO_BUF_HALFWORDS   (AUDIO_HALF_HALFWORDS * 2)

#define MP3_BUF_BYTES         4096                      /* 压缩数据缓冲区大小 */
#define MP3_FRAME_MAX         2400                      /* 单帧压缩数据最大字节数（留余量） */

#define MP3_REQUIRED_HZ       44100                     /* 当前 I2S 固定配置 */
#define MP3_REQUIRED_CHANNELS 2

static I2S_HandleTypeDef *mp3_i2s;
static FIL mp3_file;

static uint16_t Audio_Buf[AUDIO_BUF_HALFWORDS];         /* DMA 源：两个 4608 字节半区 */
static uint8_t mp3_buf[MP3_BUF_BYTES];                  /* 压缩数据缓冲 */
static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];      /* 一帧解码结果 */

static mp3dec_t mp3d;
static mp3dec_frame_info_t info;
static int mp3_len;                                     /* 缓冲内有效字节数 */
static int mp3_pos;                                     /* 下一个待解码字节位置 */
static uint8_t mp3_eof;                                 /* 文件已读完 */
static uint8_t format_checked;                          /* 首帧格式已检查 */
static uint8_t playing;
static uint8_t paused;
static char current_path[64];                           /* 重播用 */

static volatile uint8_t half_flag;                      /* 前半区播完 */
static volatile uint8_t full_flag;                      /* 后半区播完 */

/* 解码并填满一个半区。返回 1 = 正常，0 = 文件结束或出错 */
static uint8_t mp3_refill(uint8_t half)
{
    uint8_t *p = (uint8_t *)Audio_Buf + half * AUDIO_HALF_BYTES;
    uint32_t bytes_filled = 0;
    int samples;
    int left;
    UINT r;

    while (bytes_filled < AUDIO_HALF_BYTES)
    {
        /* 缓冲用完：结束则收尾，否则读新数据 */
        if (mp3_pos >= mp3_len) {
            if (mp3_eof) {
                memset(p + bytes_filled, 0, AUDIO_HALF_BYTES - bytes_filled);
                return 0;
            }
            mp3_pos = 0;
            mp3_len = 0;
            r = 0;
            f_read(&mp3_file, mp3_buf, MP3_BUF_BYTES, &r);
            if (r == 0) {
                mp3_eof = 1;
                memset(p + bytes_filled, 0, AUDIO_HALF_BYTES - bytes_filled);
                return 0;
            }
            mp3_len = (int)r;
        }

        /* 剩余不足一整帧：把尾部搬到头部，再补读，避免交界处丢半帧 */
        if (!mp3_eof && (mp3_len - mp3_pos) < MP3_FRAME_MAX) {
            left = mp3_len - mp3_pos;
            memmove(mp3_buf, mp3_buf + mp3_pos, left);
            mp3_len = left;
            mp3_pos = 0;

            r = 0;
            f_read(&mp3_file, mp3_buf + mp3_len,
                   MP3_BUF_BYTES - mp3_len, &r);
            if (r == 0) {
                mp3_eof = 1;
            } else {
                mp3_len += (int)r;
            }
            continue;
        }

        samples = mp3dec_decode_frame(&mp3d, mp3_buf + mp3_pos,
                                      mp3_len - mp3_pos, pcm, &info);

        if (samples > 0) {
            /* 首帧确认格式，当前 I2S 只支持 44.1k 立体声 */
            if (!format_checked) {
                format_checked = 1;
                if (info.hz != MP3_REQUIRED_HZ ||
                    info.channels != MP3_REQUIRED_CHANNELS) {
                    memset(p + bytes_filled, 0,
                           AUDIO_HALF_BYTES - bytes_filled);
                    return 0;
                }
            }

            /* samples 是每声道采样数，16bit 立体声所以要 ×4 */
            uint32_t pcm_bytes = samples * info.channels * 2;

            if (bytes_filled + pcm_bytes > AUDIO_HALF_BYTES) {
                pcm_bytes = AUDIO_HALF_BYTES - bytes_filled;
            }
            memcpy(p + bytes_filled, pcm, pcm_bytes);
            bytes_filled += pcm_bytes;
            mp3_pos += info.frame_bytes;
        }
        else if (info.frame_bytes > 0) {
            /* Info 帧或跳过的无效字节，继续 */
            mp3_pos += info.frame_bytes;
        }
        else {
            /* 连一个整帧都凑不出来，静音收尾 */
            mp3_eof = 1;
            memset(p + bytes_filled, 0, AUDIO_HALF_BYTES - bytes_filled);
            return 0;
        }
    }

    return 1;
}

int8_t mp3_player_play(I2S_HandleTypeDef *hi2s, const char *path)
{
    if (hi2s == NULL || path == NULL) return MP3_PLAYER_ERR_OPEN;

    mp3_i2s = hi2s;
    playing = 0;
    paused = 0;
    half_flag = 0;
    full_flag = 0;
    mp3_len = 0;
    mp3_pos = 0;
    mp3_eof = 0;
    format_checked = 0;
    mp3dec_init(&mp3d);

    strncpy(current_path, path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    if (f_open(&mp3_file, path, FA_READ) != FR_OK) {
        return MP3_PLAYER_ERR_OPEN;
    }

    /* 预装两节车厢；任何一半装不出来（文件太短/格式不支持）都算失败 */
    if (!mp3_refill(0) || !mp3_refill(1)) {
        f_close(&mp3_file);
        return MP3_PLAYER_ERR_FORMAT;
    }

    half_flag = 0;
    full_flag = 0;
    if (HAL_I2S_Transmit_DMA(mp3_i2s, Audio_Buf,
                             AUDIO_BUF_HALFWORDS) != HAL_OK) {
        f_close(&mp3_file);
        return MP3_PLAYER_ERR_DMA;
    }

    playing = 1;
    return MP3_PLAYER_OK;
}

uint8_t mp3_player_poll(void)
{
    if (!playing) return 0;

    if (half_flag) {
        half_flag = 0;
        if (!mp3_refill(0)) {
            mp3_player_stop();
            return 0;
        }
    }
    if (full_flag) {
        full_flag = 0;
        if (!mp3_refill(1)) {
            mp3_player_stop();
            return 0;
        }
    }
    return 1;
}

void mp3_player_stop(void)
{
    if (!playing) return;
    HAL_I2S_DMAStop(mp3_i2s);
    f_close(&mp3_file);
    playing = 0;
    paused = 0;
}

void mp3_player_pause(void)
{
    if (!playing || paused) return;
    HAL_I2S_DMAPause(mp3_i2s);
    paused = 1;
}

void mp3_player_resume(void)
{
    if (!playing || !paused) return;
    HAL_I2S_DMAResume(mp3_i2s);
    paused = 0;
}

uint8_t mp3_player_is_paused(void)
{
    return paused;
}

int8_t mp3_player_restart(void)
{
    if (!playing) return MP3_PLAYER_ERR_OPEN;
    mp3_player_stop();
    return mp3_player_play(mp3_i2s, current_path);
}

/* HAL 回调：只置标志，真正的解码放到主循环里做 */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (playing && hi2s == mp3_i2s) {
        half_flag = 1;
    }
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (playing && hi2s == mp3_i2s) {
        full_flag = 1;
    }
}

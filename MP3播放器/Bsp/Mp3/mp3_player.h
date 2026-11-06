#ifndef __MP3_PLAYER_H
#define __MP3_PLAYER_H

#include <stdint.h>
#include "i2s.h"

#define MP3_PLAYER_OK           0
#define MP3_PLAYER_ERR_OPEN   -1    /* 文件打不开 */
#define MP3_PLAYER_ERR_DMA    -2    /* I2S DMA 启动失败 */
#define MP3_PLAYER_ERR_FORMAT -3    /* 没有可用音频帧，或采样率/声道不支持 */

/* 打开文件、预装两个半区并启动 I2S DMA。返回 MP3_PLAYER_xxx */
int8_t mp3_player_play(I2S_HandleTypeDef *hi2s, const char *path);

/* 主循环里轮询：1 = 还在播放，0 = 已结束（内部已停 DMA、关文件） */
uint8_t mp3_player_poll(void);

/* 手动停止（重复调用安全） */
void mp3_player_stop(void);

/* 暂停/恢复：只关 DMA 请求，I2S 不停，播放位置不丢 */
void mp3_player_pause(void);
void mp3_player_resume(void);
uint8_t mp3_player_is_paused(void);

/* 从头重播当前文件 */
int8_t mp3_player_restart(void);

#endif /* __MP3_PLAYER_H */

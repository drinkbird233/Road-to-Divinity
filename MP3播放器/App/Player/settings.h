#ifndef __SETTINGS_H
#define __SETTINGS_H

#include <stdint.h>

/* 上电记忆：音量档位 + 当前曲目索引，存 SD 根目录 MP3.CFG */

uint8_t settings_load(uint8_t *vol, uint8_t *track);
uint8_t settings_save(uint8_t vol, uint8_t track);

#endif /* __SETTINGS_H */

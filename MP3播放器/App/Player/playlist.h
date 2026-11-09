#ifndef __PLAYLIST_H
#define __PLAYLIST_H

#include <stdint.h>

/* 扫描 SD 根目录生成播放列表（FatFs 当前只支持 8.3 短文件名） */
uint8_t playlist_scan(void);

uint8_t playlist_count(void);
uint8_t playlist_current(void);
void    playlist_set_current(uint8_t index);

uint8_t playlist_next(void);        /* 返回新索引 */
uint8_t playlist_prev(void);

const char *playlist_name(uint8_t index);

/* 生成 FatFs 路径，例如 "0:/SONG01.MP3" */
void playlist_path(uint8_t index, char *buf, uint16_t size);

#endif /* __PLAYLIST_H */

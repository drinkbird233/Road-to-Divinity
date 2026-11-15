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

/* 播放模式：顺序循环 / 单曲循环 / 随机 */
#define PLAYLIST_MODE_SEQ     0
#define PLAYLIST_MODE_SINGLE  1
#define PLAYLIST_MODE_SHUFFLE 2

uint8_t playlist_mode(void);
uint8_t playlist_mode_cycle(void);
void    playlist_set_mode(uint8_t mode);
const char *playlist_mode_name(void);

/* 每次重新扫描递增，供 UI 判断列表是否变化 */
uint8_t playlist_version(void);

/* 一首播完后按当前模式决定下一首 */
uint8_t playlist_advance(void);

const char *playlist_name(uint8_t index);

/* 生成 FatFs 路径，例如 "0:/SONG01.MP3" */
void playlist_path(uint8_t index, char *buf, uint16_t size);

#endif /* __PLAYLIST_H */

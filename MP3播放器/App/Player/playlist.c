#include "playlist.h"
#include "fatfs.h"
#include "main.h"
#include <string.h>

#define PLAYLIST_MAX 32

static char names[PLAYLIST_MAX][13];   /* 8.3 文件名 + '\0' */
static uint8_t count;
static uint8_t current;
static uint8_t mode = PLAYLIST_MODE_SEQ;
static uint8_t version = 1;
static uint32_t rng;

static uint8_t is_mp3(const char *name)
{
    size_t n = strlen(name);

    if (n < 4) return 0;

    const char *ext = name + n - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'm' || ext[1] == 'M') &&
            (ext[2] == 'p' || ext[2] == 'P') &&
            ext[3] == '3');
}

uint8_t playlist_scan(void)
{
    DIR dir;
    FILINFO fno;
    uint8_t i, j;
    char tmp[13];

    count = 0;
    current = 0;
    version++;

    if (f_opendir(&dir, "0:/") == FR_OK) {
        while (count < PLAYLIST_MAX &&
               f_readdir(&dir, &fno) == FR_OK &&
               fno.fname[0] != 0) {
            if (fno.fattrib & AM_DIR) continue;
            if (!is_mp3(fno.fname)) continue;

            strncpy(names[count], fno.fname, sizeof(names[0]) - 1);
            names[count][sizeof(names[0]) - 1] = '\0';
            count++;
        }
        f_closedir(&dir);
    }

    /* 按文件名排序，保证每次上电顺序一致 */
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                strcpy(tmp, names[i]);
                strcpy(names[i], names[j]);
                strcpy(names[j], tmp);
            }
        }
    }

    return count;
}

uint8_t playlist_count(void)
{
    return count;
}

uint8_t playlist_current(void)
{
    return current;
}

void playlist_set_current(uint8_t index)
{
    if (index < count) current = index;
}

uint8_t playlist_next(void)
{
    if (count == 0) return 0;
    current = (uint8_t)((current + 1) % count);
    return current;
}

uint8_t playlist_prev(void)
{
    if (count == 0) return 0;
    current = (uint8_t)((current + count - 1) % count);
    return current;
}

const char *playlist_name(uint8_t index)
{
    return (index < count) ? names[index] : "";
}

void playlist_path(uint8_t index, char *buf, uint16_t size)
{
    if (size == 0) return;

    if (index >= count) {
        buf[0] = '\0';
        return;
    }

    strncpy(buf, "0:/", size - 1);
    buf[size - 1] = '\0';
    strncat(buf, names[index], size - strlen(buf) - 1);
}

uint8_t playlist_mode(void)
{
    return mode;
}

uint8_t playlist_mode_cycle(void)
{
    mode = (uint8_t)((mode + 1) % 3);
    return mode;
}

void playlist_set_mode(uint8_t value)
{
    if (value < 3) mode = value;
}

uint8_t playlist_version(void)
{
    return version;
}

const char *playlist_mode_name(void)
{
    switch (mode) {
    case PLAYLIST_MODE_SINGLE:  return "SINGLE";
    case PLAYLIST_MODE_SHUFFLE: return "SHUFFLE";
    default:                    return "SEQUENCE";
    }
}

static uint8_t random_index(uint8_t exclude)
{
    uint8_t index;

    if (rng == 0) {
        rng = HAL_GetTick() ^ 0x9E3779B9u;
    }
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;

    index = (uint8_t)(rng % count);
    if (count > 1 && index == exclude) {
        index = (uint8_t)((index + 1) % count);
    }
    return index;
}

uint8_t playlist_advance(void)
{
    if (count == 0) return 0;

    switch (mode) {
    case PLAYLIST_MODE_SINGLE:
        return current;
    case PLAYLIST_MODE_SHUFFLE:
        current = random_index(current);
        return current;
    default:
        return playlist_next();
    }
}

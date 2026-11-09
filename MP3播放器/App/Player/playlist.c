#include "playlist.h"
#include "fatfs.h"
#include <string.h>

#define PLAYLIST_MAX 32

static char names[PLAYLIST_MAX][13];   /* 8.3 文件名 + '\0' */
static uint8_t count;
static uint8_t current;

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

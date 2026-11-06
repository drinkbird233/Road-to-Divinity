#include "app_key.h"
#include "key.h"
#include "mp3_player.h"
#include "es8388.h"
#include <stdio.h>

#define VOL_LEVELS (sizeof(VOL_TABLE) / sizeof(VOL_TABLE[0]))

static const uint8_t VOL_TABLE[] = {0, 5, 10, 15, 20, 25, 30, 33};

static uint8_t vol_index = 2;       /* 初始音量 10 */
static uint8_t last_key;            /* 上一次扫描到的键，用于只响应一次按下 */

static void volume_apply(void)
{
    uint8_t v = VOL_TABLE[vol_index];

    /* 只写不读：ES8388 的 repeated-start 读容易把 I2C 总线卡死 */
    es8388_write_reg(0x2E, v);
    es8388_write_reg(0x2F, v);

    /* Reg25 bit2 = DACMute：0 档时真正静音，其余档解除 */
    es8388_write_reg(0x19, (v == 0) ? 0x26 : 0x22);

    printf("volume = %u\r\n", (unsigned)v);
}

void app_key_init(void)
{
    key_init();
    vol_index = 2;
    last_key = KEY_NONE;
}

void app_key_poll(void)
{
    uint8_t key = Key_Scan();

    /* Key_Scan 现在只在按下瞬间返回一次事件；这里再兜一层同样的键 */
    if (key == last_key) return;
    last_key = key;
    if (key == KEY_NONE) return;

    switch (key) {
    case KEY0_PRESS:                /* 播放 / 暂停 */
        if (mp3_player_is_paused()) {
            mp3_player_resume();
            printf("resume\r\n");
        } else {
            mp3_player_pause();
            printf("pause\r\n");
        }
        break;

    case KEY1_PRESS:                /* 音量 - */
        if (vol_index > 0) vol_index--;
        volume_apply();
        break;

    case KEY2_PRESS:                /* 音量 + */
        if (vol_index < VOL_LEVELS - 1) vol_index++;
        volume_apply();
        break;

    case WKUP_PRESS:                /* 从头重播 */
        printf("replay\r\n");
        if (mp3_player_restart() != MP3_PLAYER_OK) {
            printf("replay failed\r\n");
        }
        break;

    default:
        break;
    }
}

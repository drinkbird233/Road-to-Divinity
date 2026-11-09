#include "app_key.h"
#include "key.h"
#include "es8388.h"
#include "cmsis_os.h"
#include <stdio.h>

#define VOL_LEVELS (sizeof(VOL_TABLE) / sizeof(VOL_TABLE[0]))

static const uint8_t VOL_TABLE[] = {0, 5, 10, 15, 20, 25, 30, 33};

static osMessageQueueId_t key_cmd_queue;
static uint8_t vol_index = 2;       /* 初始音量 10 */

static void volume_apply(void)
{
    uint8_t v = VOL_TABLE[vol_index];

    /* 只写不读：ES8388 的 repeated-start 读容易把 I2C 总线卡死 */
    es8388_write_reg(0x2E, v);      /* 耳机 L */
    es8388_write_reg(0x2F, v);      /* 耳机 R */
    es8388_write_reg(0x30, v);      /* 喇叭 L */
    es8388_write_reg(0x31, v);      /* 喇叭 R */

    /* Reg25 bit2 = DACMute：0 档时真正静音，其余档解除 */
    es8388_write_reg(0x19, (v == 0) ? 0x26 : 0x22);

    printf("volume = %u\r\n", (unsigned)v);
}

void app_key_queue_init(void)
{
    key_cmd_queue = osMessageQueueNew(8, sizeof(uint8_t), NULL);
}

void app_key_task(void *argument)
{
    uint8_t cmd;
    uint8_t pending = 0;            /* KEY1/KEY2 按住中，等松手或长按 */
    uint32_t press_tick = 0;
    uint8_t long_fired = 0;

    (void)argument;
    key_init();

    for (;;)
    {
        uint8_t key = Key_Scan();

        if (key == KEY0_PRESS) {
            cmd = KEY_CMD_TOGGLE_PLAY;
            osMessageQueuePut(key_cmd_queue, &cmd, 0, 0);
        } else if (key == WKUP_PRESS) {
            cmd = KEY_CMD_REPLAY;
            osMessageQueuePut(key_cmd_queue, &cmd, 0, 0);
        } else if (key == KEY1_PRESS || key == KEY2_PRESS) {
            pending = key;
            press_tick = HAL_GetTick();
            long_fired = 0;
        }

        if (pending) {
            if (!Key_IsDown(pending)) {
                /* 短按松手：调音量 */
                if (!long_fired) {
                    cmd = (pending == KEY1_PRESS) ? KEY_CMD_VOL_DOWN : KEY_CMD_VOL_UP;
                    osMessageQueuePut(key_cmd_queue, &cmd, 0, 0);
                }
                pending = 0;
            } else if (!long_fired &&
                       (HAL_GetTick() - press_tick >= 600)) {
                /* 长按：上一首 / 下一首，每次按下只触发一次 */
                cmd = (pending == KEY1_PRESS) ? KEY_CMD_PREV : KEY_CMD_NEXT;
                osMessageQueuePut(key_cmd_queue, &cmd, 0, 0);
                long_fired = 1;
            }
        }

        osDelay(10);
    }
}

uint8_t app_key_wait_cmd(uint32_t timeout_ms, uint8_t *cmd)
{
    return (osMessageQueueGet(key_cmd_queue, cmd, NULL, timeout_ms) == osOK) ? 1 : 0;
}

void app_key_volume_up(void)
{
    if (vol_index < VOL_LEVELS - 1) {
        vol_index++;
    }
    volume_apply();
}

void app_key_volume_down(void)
{
    if (vol_index > 0) {
        vol_index--;
    }
    volume_apply();
}

uint8_t app_key_volume_get(void)
{
    return vol_index;
}

void app_key_volume_set(uint8_t index)
{
    if (index < VOL_LEVELS) {
        vol_index = index;
    }
    volume_apply();
}

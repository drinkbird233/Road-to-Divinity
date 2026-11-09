#ifndef __APP_KEY_H
#define __APP_KEY_H

#include <stdint.h>

/* KeyTask 发给 AudioTask 的命令 */
#define KEY_CMD_TOGGLE_PLAY  1
#define KEY_CMD_REPLAY       2
#define KEY_CMD_VOL_UP       3
#define KEY_CMD_VOL_DOWN     4
#define KEY_CMD_NEXT         5
#define KEY_CMD_PREV         6

/* 在调度器启动前创建命令队列 */
void app_key_queue_init(void);

/* KeyTask 主体：扫描按键并发送命令 */
void app_key_task(void *argument);

/* AudioTask 等待命令；timeout_ms 用 0xFFFFFFFF 表示永久等待 */
uint8_t app_key_wait_cmd(uint32_t timeout_ms, uint8_t *cmd);

/* 音量由 AudioTask 执行，保持 I2C 只有单一使用者 */
#define APP_VOL_LEVELS 8

void app_key_volume_up(void);
void app_key_volume_down(void);
uint8_t app_key_volume_get(void);
void app_key_volume_set(uint8_t index);

#endif /* __APP_KEY_H */

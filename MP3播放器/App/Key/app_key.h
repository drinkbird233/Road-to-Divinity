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
#define KEY_CMD_MODE         7
#define KEY_CMD_SELECT       8
#define KEY_CMD_SET_MODE     9
#define KEY_CMD_SET_VOLUME  10
#define KEY_CMD_RESCAN      11
#define KEY_CMD_SEEK        12
#define KEY_CMD_SET_OUTPUT  13

typedef struct {
    uint8_t cmd;
    uint32_t value;
} key_cmd_t;

/* 在调度器启动前创建命令队列 */
void app_key_queue_init(void);

/* KeyTask 主体：扫描按键并发送命令 */
void app_key_task(void *argument);

/* AudioTask 等待命令；timeout_ms 用 0xFFFFFFFF 表示永久等待 */
uint8_t app_key_wait_cmd(uint32_t timeout_ms, key_cmd_t *msg);

/* UI 等其他模块发送命令 */
int8_t app_key_send_cmd(uint8_t cmd, uint32_t value);

/* 音量由 AudioTask 执行，保持 I2C 只有单一使用者 */
#define APP_VOL_LEVELS 8

void app_key_volume_up(void);
void app_key_volume_down(void);
uint8_t app_key_volume_get(void);
void app_key_volume_set(uint8_t index);

#endif /* __APP_KEY_H */

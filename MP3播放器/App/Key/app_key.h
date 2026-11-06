#ifndef __APP_KEY_H
#define __APP_KEY_H

/* 按键业务层：把 Bsp/Key 的扫描结果映射成播放/暂停、音量、重播等动作 */

void app_key_init(void);
void app_key_poll(void);

#endif /* __APP_KEY_H */

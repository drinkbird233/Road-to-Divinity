#ifndef __KEY_H
#define __KEY_H

#include "main.h"

#define KEY_NONE    0
#define KEY0_PRESS  1
#define KEY1_PRESS  2
#define KEY2_PRESS  3
#define WKUP_PRESS  4

void key_init(void);
uint8_t Key_Scan(void);

/* 查询某个键当前是否仍按住（用于长按判断） */
uint8_t Key_IsDown(uint8_t key);

#endif

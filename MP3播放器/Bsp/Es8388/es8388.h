#ifndef __ES8388_H
#define __ES8388_H

#include "main.h"

#define ES8388_ADDR 0x10 /* ES8388 的 7 位器件地址 */

/* 初始化：传入 I2C 句柄，内部保存下来供读写寄存器用 */
uint8_t es8388_init(I2C_HandleTypeDef *hi2c);

uint8_t es8388_write_reg(uint8_t reg, uint8_t val);
uint8_t es8388_read_reg(uint8_t reg);

void es8388_adda_cfg(uint8_t dacen, uint8_t adcen);
void es8388_output_cfg(uint8_t o1en, uint8_t o2en);
void es8388_hpvol_set(uint8_t volume);
void es8388_spkvol_set(uint8_t volume);

#endif

/**
 * @file    es8388.c
 * @brief   ES8388 驱动（I2C 配置）
 * @note    寄存器序列参考正点原子官方例程，逐条注释了作用；
 *          手册里的寄存器号是十进制，代码里用十六进制，列注释时两个都标出来。
 */

#include "es8388.h"

/* I2C 句柄：由 es8388_init() 传入并保存在这里 */
static I2C_HandleTypeDef *g_es8388_i2c = NULL;

/* ES8388 的 7 位地址是 0x10，HAL 需要 8 位地址，左移一位 */
#define ES8388_ADDR8  (ES8388_ADDR << 1)

static void i2c_delay(void)
{
    for (volatile uint32_t i = 0; i < 200; i++) { }
}

/* I2C 总线被从机拉住 SDA 时，用 9 个时钟脉冲 + STOP 释放，再重新初始化外设 */
static void es8388_i2c_recover(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint32_t i;

    HAL_I2C_DeInit(g_es8388_i2c);

    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    for (i = 0; i < 9; i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
        i2c_delay();
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
        i2c_delay();
    }
    /* STOP：SCL 高时 SDA 由低变高 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    i2c_delay();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    i2c_delay();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    i2c_delay();

    HAL_I2C_Init(g_es8388_i2c);
}

/**
 * @brief  写一个寄存器
 * @param  reg 寄存器地址
 * @param  val 要写入的值
 * @retval 0 成功，1 没初始化，2 I2C 失败
 */
uint8_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    if (g_es8388_i2c == NULL)
    {
        return 1;
    }

    if (HAL_I2C_Mem_Write(g_es8388_i2c, ES8388_ADDR8, reg,
                          I2C_MEMADD_SIZE_8BIT, &val, 1, 100) != HAL_OK)
    {
        es8388_i2c_recover();

        if (HAL_I2C_Mem_Write(g_es8388_i2c, ES8388_ADDR8, reg,
                              I2C_MEMADD_SIZE_8BIT, &val, 1, 100) != HAL_OK)
        {
            return 2;
        }
    }

    return 0;
}

/**
 * @brief  读一个寄存器
 * @param  reg 寄存器地址
 * @retval 读到的值，失败返回 0
 */
uint8_t es8388_read_reg(uint8_t reg)
{
    uint8_t val = 0;

    if (g_es8388_i2c == NULL)
    {
        return 0;
    }

    if (HAL_I2C_Mem_Read(g_es8388_i2c, ES8388_ADDR8, reg,
                         I2C_MEMADD_SIZE_8BIT, &val, 1, 100) != HAL_OK)
    {
        /* 读失败同样做总线恢复，避免一次异常拖死后续所有操作 */
        es8388_i2c_recover();
        return 0;
    }

    return val;
}

/**
 * @brief  ES8388 初始化
 * @param  hi2c 已初始化的 I2C 句柄（本板为 &hi2c1）
 * @retval 0 成功
 */
uint8_t es8388_init(I2C_HandleTypeDef *hi2c)
{
    if (hi2c == NULL)
    {
        return 1;
    }
    g_es8388_i2c = hi2c;

    /* ---------- 1. 软复位 ---------- */
    es8388_write_reg(0x00, 0x80);          /* Reg0 bit7 SCPReset=1，触发软复位 */
    es8388_write_reg(0x00, 0x00);          /* 清掉复位位 */
    HAL_Delay(100);                        /* 等芯片复位完成，期间写入会被忽略 */

    /* ---------- 2. 模拟上电 ---------- */
    es8388_write_reg(0x01, 0x58);          /* Reg1 Chip Control 2，上电第一拍 */
    es8388_write_reg(0x01, 0x50);          /* Reg1 上电第二拍 */
    es8388_write_reg(0x02, 0xF3);          /* Reg2 Chip Power Management，上电第一拍 */
    es8388_write_reg(0x02, 0xF0);          /* Reg2 上电第二拍 */

    /* ---------- 3. 参考电压与 ADC 关断 ---------- */
    es8388_write_reg(0x03, 0x09);          /* Reg3 ADC Power Management，关掉 ADC 电源 */
    es8388_write_reg(0x00, 0x06);          /* Reg0 EnRef=1、VMIDSEL=10，使能参考电压，Vmid 用 500k 分压 */

    /* ---------- 4. DAC 上电，输出先全部关着 ---------- */
    es8388_write_reg(0x04, 0x00);          /* Reg4 bit7/6=0 左右 DAC 上电；bit5~2=0 四个输出全关 */
    es8388_write_reg(0x08, 0x00);          /* Reg8 MCLK 不分频（芯片做从机，时钟由 STM32 给） */
    es8388_write_reg(0x2B, 0x80);          /* Reg43 DACLRC 与 ADCLRC 相同 */

    /* ---------- 5. 录音（ADC）相关，播放用不到，官方序列保留 ---------- */
    es8388_write_reg(0x09, 0x88);          /* Reg9 ADC PGA 增益 +24dB */
    es8388_write_reg(0x0C, 0x4C);          /* Reg12 ADC 数据 16bit */
    es8388_write_reg(0x0D, 0x02);          /* Reg13 ADC 的 MCLK/采样率 = 256 */
    es8388_write_reg(0x10, 0x00);          /* Reg16 ADC 数字音量 L */
    es8388_write_reg(0x11, 0x00);          /* Reg17 ADC 数字音量 R */

    /* ---------- 6. DAC 格式与数字音量 ---------- */
    es8388_write_reg(0x17, 0x18);          /* Reg23 DAC 数据 16bit，飞利浦 I2S 格式 */
    es8388_write_reg(0x18, 0x02);          /* Reg24 DAC 的 MCLK/采样率 = 256 */
    es8388_write_reg(0x1A, 0x00);          /* Reg26 DAC 数字音量 L */
    es8388_write_reg(0x1B, 0x00);          /* Reg27 DAC 数字音量 R */

    /* ---------- 7. 混音 ---------- */
    es8388_write_reg(0x27, 0xB8);          /* Reg39 左混频器 */
    es8388_write_reg(0x2A, 0xB8);          /* Reg42 右混频器 */
    HAL_Delay(100);

    return 0;
}

/**
 * @brief  DAC/ADC 开关
 * @param  dacen 1 开 DAC，0 关
 * @param  adcen 1 开 ADC，0 关
 * @note   Reg2 的位是“1 = 关”，所以要用取反来填
 */
void es8388_adda_cfg(uint8_t dacen, uint8_t adcen)
{
    uint8_t val = 0;

    val |= ((!dacen) << 0);
    val |= ((!adcen) << 1);
    val |= ((!dacen) << 2);
    val |= ((!adcen) << 3);

    es8388_write_reg(0x02, val);
}

/**
 * @brief  输出通道开关
 * @param  o1en 通道 1 使能（LOUT1/ROUT1，耳机）
 * @param  o2en 通道 2 使能（LOUT2/ROUT2，喇叭）
 * @note   Reg4：通道 1 是 bit5/4，通道 2 是 bit3/2，1 = 使能
 */
void es8388_output_cfg(uint8_t o1en, uint8_t o2en)
{
    uint8_t val = 0;

    val |= o1en * (3 << 4);                /* LOUT1 + ROUT1 */
    val |= o2en * (3 << 2);                /* LOUT2 + ROUT2 */

    es8388_write_reg(0x04, val);
}

/**
 * @brief  耳机音量
 * @param  volume 0~33
 */
void es8388_hpvol_set(uint8_t volume)
{
    if (volume > 33)
    {
        volume = 33;
    }

    es8388_write_reg(0x2E, volume);        /* Reg46 LOUT1 音量 */
    es8388_write_reg(0x2F, volume);        /* Reg47 ROUT1 音量 */
}

/**
 * @brief  喇叭音量
 * @param  volume 0~33
 */
void es8388_spkvol_set(uint8_t volume)
{
    if (volume > 33)
    {
        volume = 33;
    }

    es8388_write_reg(0x30, volume);        /* Reg48 LOUT2 音量 */
    es8388_write_reg(0x31, volume);        /* Reg49 ROUT2 音量 */
}

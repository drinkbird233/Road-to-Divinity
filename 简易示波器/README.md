# 简易数字示波器（STM32F103C8T6）

基于立创「简易示波器」板卡实现的手持示波器。ADC 连续采样 + DMA 搬运，软件触发对齐波形，输入捕获测频，1.8 寸 ST7735S 屏显示。

## 功能

- 波形显示：ADC 采集 300 点，软件触发对齐，左侧绘制 100 列波形
- 频率测量：TIM3 输入捕获（PWM 输入/复位模式），自动换算 Hz / kHz
- 峰峰值显示：帧内最大最小值，屏上显示电压
- 方波信号源：TIM2_CH3 输出 1K/2K/4K 方波，可自检
- 交互：旋钮缩放时间轴，按键调频率/占空比/输出开关，KEYD 暂停
- LED 指示：PC14 / PC15

## 硬件引脚

```
TFT : SCL=PA5  SDA=PA7  RST=PB5  DC=PB6  CS=PB7  BLK=PB8
ADC : PA3                    测频: PA6 (TIM3_CH1)
方波: PA2 (TIM2_CH3)         按键: PB13 / PB14 / PB15 / PB9(KEYD)
旋钮: PB4(A) / PB3(B)        LED : PC14 / PC15
SWD : PA13 / PA14
```

## 构建

工具链：STM32CubeMX + CMake + Ninja + arm-none-eabi-gcc

```bash
cmake --preset Debug
cmake --build build/Debug
```

## 烧录

```bash
STM32_Programmer_CLI -c port=SWD -w build/Debug/Project1.elf -v -rst
```

## 文档

- [项目说明](简易示波器-项目说明.md)：架构、CubeMX 配置、触发/缩放原理
- [开发记录](简易示波器-开发记录.md)：开发过程、踩坑与解法

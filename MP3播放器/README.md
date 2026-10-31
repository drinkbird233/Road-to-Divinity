# MP3 播放器（STM32F407ZGT6）

基于正点原子探索者 V3 的独立 MP3 播放器，HAL + CubeMX 开发，定位比赛与作品集。

## 当前功能

- SDIO 4 位 + FatFs 挂载 SD 卡，列目录、读文件
- ES8388（I2C 控制）+ I2S2 + DMA 双缓冲播放 WAV
- 44.1kHz / 16bit / 立体声，实测采样率误差约 0.24%
- USART1 115200 调试输出；PF8 蜂鸣器、PF9/PF10 LED

## 状态

- WAV 播放全链路已打通
- 下一步：minimp3 解码 MP3、模块化、FreeRTOS、选歌与 LVGL 界面

## 硬件

- MCU：STM32F407ZGT6（探索者 V3，HSE 8MHz，168MHz）
- 音频：ES8388；I2C1 SCL PB8 / SDA PB9；I2S2 LRCK PB12 / SCLK PB13 / SD PC3 / MCLK PC6
- 存储：SDIO PC8-PC12 + PD2
- 调试：USART1 115200

## 构建

Keil MDK 打开 `MDK-ARM/MyMP3.uvprojx`，CMSIS-DAP 下载。首次下载需在 Flash Download 里勾选 Reset and Run，否则程序下载后不运行。

## 说明

架构参考 GitHub 原型 `sakabu-ljh/music_player`，本工程为独立重写，未在其基础上修改。卡上的音频文件不入库。

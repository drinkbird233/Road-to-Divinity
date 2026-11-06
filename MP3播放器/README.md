# MP3 播放器（STM32F407ZGT6）

基于正点原子探索者 V3 的独立 MP3 播放器，HAL + CubeMX 开发，定位比赛与作品集。

## 当前功能

- SDIO 4 位 + FatFs 挂载 SD 卡，列目录、读文件
- ES8388（I2C 控制）+ I2S2 + DMA 双缓冲播放 WAV
- minimp3 解码 MP3 → PCM → I2S DMA 双缓冲播放（`Bsp/Mp3/`）
- FreeRTOS（CMSIS-V2）：AudioTask 负责挂载、解码与喂数据
- 四键控制：KEY0 播放/暂停、KEY1 音量-、KEY2 音量+、WK_UP 重播
- 44.1kHz / 16bit / 立体声，实测采样率误差约 0.24%
- USART1 115200 调试输出；PF8 蜂鸣器、PF9/PF10 LED

## 状态

- WAV / MP3 播放全链路已打通，FreeRTOS 下真机播放正常
- 按键控制、音量（含 0 档 DAC 静音）已完成
- 下一步：采样率自适应、选歌列表与 LVGL 界面

## 模块接口

`mp3_player_play(hi2s, path)` 打开文件、预装两个半区并启动 I2S DMA；`mp3_player_poll()` 在主循环中调用，返回 1 表示仍在播放；`mp3_player_stop()` 手动停止；另有 `pause/resume/is_paused/restart`。I2S 半完成/完成回调也由该模块实现。按键业务在 `App/Key/`，底层扫描在 `Bsp/Key/`。

## 实现要点

- MP3 压缩数据缓冲 4KB，用 `mp3_len / mp3_pos / mp3_eof` 记录读取状态；解码前保证剩余 ≥ 2400 字节，不足时把尾部 `memmove` 到缓冲头再补读，避免缓冲交界处丢半帧
- PCM 半区 4608 字节（1152 采样 × 2 声道 × 2 字节），一帧整对齐，双缓冲共 9216 字节
- FreeRTOS：AudioTask 栈 24KB（容纳 minimp3 约 16KB 工作区），FreeRTOS 堆 40KB，HAL 时基改为 TIM6
- SD 读写使用 DMA + RTOS 模板，`sd_diskio.c` 打开 `ENABLE_SCRATCH_BUFFER`，避免 Word DMA 写入非 4 字节对齐的 FatFs 缓冲导致数据错位
- ES8388 音量只写不读；写失败时执行 9 时钟 + STOP 的 I2C 总线恢复并重试；音量 0 档通过 `Reg25` DACMute 实现真静音
- 限制：当前 I2S 固定 44.1kHz 立体声，其他采样率/单声道未适配；FatFs `_USE_LFN=0`，仅支持 8.3 短文件名；歌曲播完后 AudioTask 挂起，需复位重新播放

## 硬件

- MCU：STM32F407ZGT6（探索者 V3，HSE 8MHz，168MHz）
- 音频：ES8388；I2C1 SCL PB8 / SDA PB9；I2S2 LRCK PB12 / SCLK PB13 / SD PC3 / MCLK PC6
- 存储：SDIO PC8-PC12 + PD2
- 调试：USART1 115200

## 构建

Keil MDK 打开 `MDK-ARM/MyMP3.uvprojx`，CMSIS-DAP 下载。首次下载需在 Flash Download 里勾选 Reset and Run，否则程序下载后不运行。

## 说明

架构参考 GitHub 原型 `sakabu-ljh/music_player`，本工程为独立重写，未在其基础上修改。MP3 解码使用 public domain 的 [minimp3](https://github.com/lieff/minimp3)（见 `Bsp/Mp3/LICENSE`）。卡上的音频文件不入库。

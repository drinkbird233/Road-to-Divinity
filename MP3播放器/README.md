# MP3 播放器（STM32F407ZGT6）

基于正点原子探索者 V3 的独立 MP3 播放器，HAL + CubeMX 开发，定位比赛与作品集。

## 当前功能

- SDIO 4 位 + FatFs 挂载 SD 卡，列目录、读文件
- ES8388（I2C 控制）+ I2S2 + DMA 双缓冲播放，耳机与板载喇叭同时输出
- minimp3 解码 MP3 → PCM → I2S DMA 双缓冲播放（`Bsp/Mp3/`）
- FreeRTOS（CMSIS-V2）双任务：AudioTask 负责播放与 ES8388，KeyTask 负责按键，命令队列通信
- 播放列表：扫描 SD 根目录 MP3，自动连播、循环；单曲失败自动跳下一首
- 四键控制：KEY0 播放/暂停；KEY1/KEY2 短按音量±、长按 600ms 上一首/下一首；WK_UP 重播
- 上电记忆：音量档位与当前曲目存内部 Flash，断电保持
- 44.1kHz / 16bit / 立体声，实测采样率误差约 0.24%
- USART1 115200 调试输出；PF8 蜂鸣器、PF9/PF10 LED

## 状态

- WAV / MP3 播放全链路已打通，FreeRTOS 下真机播放正常
- 双任务、播放列表、长按切歌、双通道输出、Flash 记忆均已完成
- 下一步：采样率/声道自适应，之后做 LVGL 界面

## 模块接口

- `Bsp/Mp3/mp3_player.c`：`play / poll / stop / pause / resume / is_paused / restart`
- `App/Key/app_key.c`：KeyTask、命令队列、音量控制
- `App/Player/playlist.c`：根目录 MP3 扫描、排序、上下曲
- `App/Player/settings.c`：Flash Sector 11 追加日志，保存音量与曲目索引

## 实现要点

- MP3 压缩数据缓冲 4KB，用 `mp3_len / mp3_pos / mp3_eof` 记录读取状态；解码前保证剩余 ≥ 2400 字节，不足时把尾部 `memmove` 到缓冲头再补读，避免缓冲交界处丢半帧
- PCM 半区 4608 字节（1152 采样 × 2 声道 × 2 字节），一帧整对齐，双缓冲共 9216 字节
- FreeRTOS：AudioTask 栈 24KB（容纳 minimp3 约 16KB 工作区），KeyTask 1KB，堆 40KB，HAL 时基 TIM6
- AudioTask 播放结束进入待机并阻塞在命令队列上，不挂起；按键始终有效
- SD 读写使用 DMA + RTOS 模板，`sd_diskio.c` 打开 `ENABLE_SCRATCH_BUFFER`，避免 Word DMA 写入非 4 字节对齐的 FatFs 缓冲导致数据错位
- ES8388 音量只写不读；写失败时执行 9 时钟 + STOP 的 I2C 总线恢复并重试；音量 0 档通过 `Reg25` DACMute 实现真静音
- 设置保存在 Flash `0x080E0000` 起的 Sector 11，每条 16 字节，约 8000 条后上电时擦除；要求程序 < 0xE0000
- 限制：I2S 固定 44.1kHz 立体声；FatFs `_USE_LFN=0`，仅支持 8.3 短文件名；播放列表最多 32 首；长按切歌一次只跳一首

## 硬件

- MCU：STM32F407ZGT6（探索者 V3，HSE 8MHz，168MHz）
- 音频：ES8388；I2C1 SCL PB8 / SDA PB9；I2S2 LRCK PB12 / SCLK PB13 / SD PC3 / MCLK PC6
- 存储：SDIO PC8-PC12 + PD2
- 调试：USART1 115200

## 构建

Keil MDK 打开 `MDK-ARM/MyMP3.uvprojx`，CMSIS-DAP 下载。首次下载需在 Flash Download 里勾选 Reset and Run，否则程序下载后不运行。

## 说明

架构参考 GitHub 原型 `sakabu-ljh/music_player`，本工程为独立重写，未在其基础上修改。MP3 解码使用 public domain 的 [minimp3](https://github.com/lieff/minimp3)（见 `Bsp/Mp3/LICENSE`）。卡上的音频文件不入库。

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
- 采样率自适应：8k / 11.025k / 12k / 16k / 22.05k / 24k / 32k / 44.1k / 48k；单声道自动复制为立体声
- LVGL 9.4 音乐播放器界面（320×240 横屏）：音乐库、播放详情、播放模式、设置页
- XPT2046 电阻触摸，二维仿射校准；UI 点歌、播放/暂停、上下曲、模式、音量、重扫均控制真实播放器
- USART1 115200 调试输出；PF8 蜂鸣器、PF9/PF10 LED

## 状态

- MP3 播放、播放列表、采样率/声道自适应、Flash 记忆、LVGL UI 与触摸均已完成
- 待完善：进度条 seek、耳机/喇叭切换、亮度调节、屏幕休眠
- 下一步：完善上述设置项，再考虑歌词/频谱等扩展

## 模块接口

- `Bsp/Mp3/mp3_player.c`：`play / poll / stop / pause / resume / is_paused / restart`，以及采样率、位置、时长查询
- `App/Key/app_key.c`：KeyTask、命令队列、音量控制
- `App/Player/playlist.c`：根目录 MP3 扫描、排序、上下曲、播放模式
- `App/Player/settings.c`：Flash Sector 11 追加日志，保存音量与曲目索引
- `App/Player/player_adapter.c`：把参考 UI 的 `player_send / player_get_snapshot` 映射到现有播放器
- `Bsp/Touch/xpt2046.c`：XPT2046 软件 SPI 与仿射校准
- `App/Ui/`：LVGL 9.4 界面（Generated/Controller/Port/Assets）

## 实现要点

- MP3 压缩数据缓冲 4KB，用 `mp3_len / mp3_pos / mp3_eof` 记录读取状态；解码前保证剩余 ≥ 2400 字节，不足时把尾部 `memmove` 到缓冲头再补读，避免缓冲交界处丢半帧
- PCM 半区 4608 字节（1152 采样 × 2 声道 × 2 字节），一帧整对齐，双缓冲共 9216 字节
- FreeRTOS：AudioTask 栈 24KB（容纳 minimp3 约 16KB 工作区），KeyTask 1KB，堆 40KB，HAL 时基 TIM6
- AudioTask 播放结束进入待机并阻塞在命令队列上，不挂起；按键始终有效
- SD 读写使用 DMA + RTOS 模板，`sd_diskio.c` 打开 `ENABLE_SCRATCH_BUFFER`，避免 Word DMA 写入非 4 字节对齐的 FatFs 缓冲导致数据错位
- ES8388 音量只写不读；写失败时执行 9 时钟 + STOP 的 I2C 总线恢复并重试；音量 0 档通过 `Reg25` DACMute 实现真静音
- 设置保存在 Flash `0x080E0000` 起的 Sector 11，每条 16 字节，约 8000 条后上电时擦除；要求程序 < 0xE0000
- 采样率自适应：44.1k 家族（44100/22050/11025）用 PLLI2S 271/2=135.5MHz；48k 家族（48000/32000/24000/16000/12000/8000）用 344/2=172MHz；切家族时重配 PLL，再让 HAL 重算 I2SDIV/ODD。ES8388 的 MCLK/fs 固定 256，无需改寄存器
- 单声道在 `mp3_refill` 中原地倒序展开为左右交错（`pcm[2i]=pcm[2i+1]=pcm[i]`）
- LVGL 9.4，堆 48KB 放在 CCM `0x10000000`（`LV_MEM_ADR`），显示缓冲 320×16 行在主 SRAM；LVGL UI 任务 8KB 栈
- 屏幕通过 FSMC 以横屏 320×240 驱动（MADCTL 0x68），LVGL `flush_cb` 直接调用 `lcd_flush_area`
- 触摸 XPT2046 软件 SPI（SCK PB0 / MISO PB2 / MOSI PF11 / CS PC13 / PEN PB1），四角采样后拟合二维仿射系数，中心点误差 < 2px
- UI 命令通过 `key_cmd_t` 队列进入 AudioTask，与按键共用同一条控制路径
- 进度由 mp3_player 统计已播放帧数和文件大小/码率估算时长；VBR 文件进度为近似值
- ARMCC 5 不识别 UTF-8 源码，UI 中的中文已转换为八进制转义，运行时仍为 UTF-8
- 限制：FatFs `_USE_LFN=0`，仅支持 8.3 短文件名；播放列表最多 32 首；长按切歌一次只跳一首；seek/亮度/输出切换/休眠暂未实现

## 硬件

- MCU：STM32F407ZGT6（探索者 V3，HSE 8MHz，168MHz）
- 音频：ES8388；I2C1 SCL PB8 / SDA PB9；I2S2 LRCK PB12 / SCLK PB13 / SD PC3 / MCLK PC6
- 显示：2.8 寸 240×320 ST7789，FSMC NE4/A6，CS PG12 / RS PF12 / WR PD5 / RD PD4 / BL PB15
- 触摸：XPT2046 软件 SPI，SCK PB0 / MISO PB2 / MOSI PF11 / CS PC13 / PEN PB1
- 存储：SDIO PC8-PC12 + PD2
- 调试：USART1 115200

## 构建

Keil MDK 打开 `MDK-ARM/MyMP3.uvprojx`，CMSIS-DAP 下载。首次下载需在 Flash Download 里勾选 Reset and Run，否则程序下载后不运行。

## 说明

架构参考 GitHub 原型 `sakabu-ljh/music_player`，本工程为独立重写，未在其基础上修改。MP3 解码使用 public domain 的 [minimp3](https://github.com/lieff/minimp3)（见 `Bsp/Mp3/LICENSE`）。卡上的音频文件不入库。

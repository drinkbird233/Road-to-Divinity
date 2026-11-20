# 校赛智能车 · 软件骨架（v0.1）

STM32F407ZGT6 + 8 路循迹 + HWT101 + K230 + TB6612 + HC06
技术栈：**CubeMX + HAL + FreeRTOS(CMSIS-RTOS2) + 纯 C99 算法层**

配套文档：

- [`docs/01_architecture.md`](docs/01_architecture.md) —— 技术栈论证、任务划分、CPU 预算、风险
- [`docs/02_track_strategy.md`](docs/02_track_strategy.md) —— 地图解读、运行模式、三级串级、元素状态机
- [`docs/03_bringup.md`](docs/03_bringup.md) —— 分阶段验收、调参顺序、对 `pin_plan.md` 的评审意见
- [`docs/04_keil_setup.md`](docs/04_keil_setup.md) —— **CubeMX + Keil 落地手册**（含两个必踩的坑）
- [`docs/05_open_source.md`](docs/05_open_source.md) —— 可用开源项目清单、怎么用、以及**别抄什么**

> ⚠️ 这份骨架是**在纯静态环境下手写的**，没有经过编译验证。
> 预期能直接编译通过，但第一次编译请预留 30 分钟处理 `main.h` 引脚宏命名和 HAL 版本差异。

---

## 1 目录结构

```
fw/
├─ app/
│  ├─ app_config.h     所有可调参数（唯一参数中心）
│  ├─ app_types.h      状态快照 + seqlock + 参数结构
│  ├─ app_main.[ch]    初始化、任务创建、TIM6 节拍回调
├─ ctrl/
│  ├─ ctrl_core.[ch]   5 ms 主控：里程计、串级控制、失效保护
│  └─ ctrl_fsm.[ch]    元素状态机：直道/弯/S弯/路口/盲走区/两种环岛
├─ bsp/
│  ├─ bsp_motor.[ch]   TB6612：方向、PWM、斜坡、STBY
│  ├─ bsp_encoder.[ch] TIM3/TIM4 编码器 → 速度
│  ├─ bsp_track.[ch]   PD3/PD4 软 I2C 读 0x12/0x30 + 特征提取
│  ├─ bsp_imu.[ch]     HWT101 帧解析 → yaw / gyro_z
│  ├─ bsp_vision.[ch]  K230 帧解析 → target_x/y
│  ├─ bsp_uart.[ch]    串口 DMA 环形收 + 非阻塞发
│  └─ bsp_ui.[ch]      按键/LED/蜂鸣器/电压/OLED 接口
├─ lib/
│  ├─ pid.[ch]         PID + 一阶低通 + 斜率限制（无 HAL 依赖）
│  └─ proto.[ch]       CRC8 + 0xAA55 帧解析/组帧（无 HAL 依赖）
└─ docs/               01 架构 / 02 赛道 / 03 验收 / 04 Keil / 05 开源
```

**依赖方向严格单向**：`app → ctrl → bsp → HAL`，`lib` 被引用但不反向依赖。
`lib/` 和 `ctrl/` 里不允许出现 `stm32f4xx_hal.h` —— 算法层要能在 PC 上编译仿真。

---

## 2 怎么并进 CubeMX 工程

### 2.1 CubeMX 里必须先配对的 8 件事

| # | 配置项 | 值 | 为什么 |
|---|---|---|---|
| 1 | Timebase Source | **TIM7** | FreeRTOS 占 SysTick；TIM6 要留给 5 ms 控制节拍 |
| 2 | TIM6 | PSC=8399, ARR=49, 更新中断 | 84 MHz/8400/50 = **200 Hz = 5 ms** |
| 3 | NVIC 分组 | `NVIC_PRIORITYGROUP_4`（4 位抢占） | HAL 默认，FreeRTOS 要求 |
| 4 | TIM6 中断优先级 | 抢占 6，子优先级 0 | 必须 ≥ 5 才能调 `FromISR` |
| 5 | USART3/USART6/USART2 RX DMA | Mode = **Circular** | `bsp_uart.c` 用 NDTR 差值取数，必须环形 |
| 6 | DMA 中断优先级 | 抢占 6～7 | 同上 |
| 7 | FreeRTOS | CMSIS_V2，TICK=1000，HEAP=24 KB | 见 `docs/01` §3.2 |
| 8 | 引脚 User Label | 见 §2.2 | 代码里的宏直接用这些名字 |

### 2.2 需要打的引脚标签（User Label）

`app_config.h` 和 `bsp` 层依赖 CubeMX 生成的 `_Pin` / `_GPIO_Port` 宏，标签必须**逐字一致**：

```
MOTOR_L_PWM   MOTOR_L_IN1   MOTOR_L_IN2   MOTOR_STBY
MOTOR_R_PWM   MOTOR_R_IN1   MOTOR_R_IN2
MOTOR_L_INVERT / MOTOR_R_INVERT   ← 软件宏，不是引脚
TRACK_SCL     TRACK_SDA
LED1   LED2   KEY1   KEY2   KEY3   BUZZER
BT_STATE
```

### 2.3 接入步骤

1. CubeMX 生成工程（Keil 或 CMake 都行）。
2. 把 `app/ ctrl/ bsp/ lib/` 五个目录加进 include path 和源文件列表。
3. 在 `main.c` 里加 `#include "app_main.h"`，然后：

```c
/* USER CODE BEGIN 2 */
app_init();        /* 见 app_main.c */
/* USER CODE END 2 */

/* USER CODE BEGIN RTOS_THREADS */
app_start();       /* 创建任务；调度器仍由 CubeMX 生成的 osKernelStart() 启动 */
/* USER CODE END RTOS_THREADS */
```

4. 如果 `main.c` 里已经有 `HAL_TIM_PeriodElapsedCallback` / `HAL_UART_RxCpltCallback`，
   **删掉**，用 `app_main.c` 里的版本（它用 `htim->Instance` 区分 TIM6/TIM7）。
5. `printf` 重定向到 USART1 DMA：

```c
int __io_putchar(int ch) { uint8_t c = ch; bsp_uart_send_blocking(&g_uart_dbg, &c, 1); return ch; }
```

---

## 3 上电时序（代码已实现，别改）

```
上电
 └─ GPIO 全部拉到安全态（STBY=0, PWM=0, IN1=IN2=0）
    └─ 外设初始化（DMA 先于 UART 使能）
       └─ 等 IMU 出第一帧（最多 500 ms，超时继续跑并置 FAULT_IMU_LOST）
          └─ 启动调度器
             └─ ctrl 任务启动 TIM6 → 状态机 INIT → IDLE
                └─ 收到 START / 按 KEY1 → 才使能 STBY
```

**电机在上电后到 START 之前一直是断的。** 这是硬规矩，不要为了"调试方便"提前开 STBY。

---

## 4 骨架里已经写死的东西（有意为之）

| 决定 | 位置 | 理由 |
|---|---|---|
| 控制周期 5 ms | `app_config.h` `CFG_CTRL_PERIOD_MS` | 见 `docs/02` §3.2 |
| 控制任务由 TIM6 通知唤醒 | `app_main.c` | 避免 tick 抖动 |
| 循迹读取在控制任务内 | `ctrl_core.c` | 零延迟 |
| 速度环 = 前馈 + PI | `ctrl_core.c` | 抗扰最好 |
| 转向 = 位置环 + 角速度环 | `ctrl_core.c` | 陀螺仪进内环 |
| 状态机 2 拍跑一次（10 ms） | `ctrl_core.c` | 与 5 ms 解耦 |
| 视觉只出前馈，不进反馈 | `ctrl_core.c` | 80 ms 延迟 |
| 所有共享数据走 seqlock | `app_types.h` | 无锁、无优先级反转 |
| **盲走区 = 屏蔽 8 路 + yaw 保持 + 距离预算** | `ctrl_fsm.c` `EL_BLIND` | 唯一"无反馈"路段，必须有预算兜底 |
| **环岛用累计 yaw 判出口，不用 wrap180** | `ctrl_fsm.c` `s_yaw_acc` | wrap180 返回 ±180，330°/360° 判据永远不成立 |
| **环岛"有边线跟线、无边线用偏置"** | `ctrl_fsm.c` `EL_LOOP_RUN` | 六边形有实线边，硬套偏置会被拽出去 |
| **盲走/路口/环岛不算"丢线故障"** | `ctrl_core.c` | 否则会在半路被失效保护强行停车 |

---

## 5 待你填的 TODO（`app_config.h` 里标了 `[TODO]`）

```bash
grep -rn "\[TODO\]" fw/
```

最重要的 9 个：

1. `CFG_ENCODER_PPR` / `CFG_GEAR_RATIO` / `CFG_WHEEL_DIAMETER_MM` —— JGB37-520 常见 11 线，也有 13 线版本，**手转一圈数脉冲**
2. `CFG_TRACK_BLACK_LEVEL` —— 实测黑线是 0 还是 1
3. `CFG_IMU_GYRO_LSB` —— HWT101 的角速度量纲（0.01 还是 0.1 dps/LSB）
4. `CFG_MOTOR_L_INVERT` / `CFG_MOTOR_R_INVERT`
5. `CFG_IMU_BAUD` —— 改成 115200 后同步改
6. `CFG_BLIND_MAX_MM` —— **盲走区断口长度 + 200 余量**，这是防出界的唯一保险
7. `CFG_VIS_TAU_MS` —— 视觉延迟实测值
8. `ctrl_fsm.c` 里各元素的里程窗与阈值（尤其 `FSM_LOOP_ENTRY_MM`、`FSM_CENTER_ERR`）
9. `bsp_ui.c` 的 OLED 驱动（骨架里只留接口，接你现成的 SSD1306 库）

---

## 6 下一步建议

1. **先做 P0–P2**（`docs/03_bringup.md`），把电机和编码器跑准。
2. 同时可以并行做一件事：**把这套控制律在 PC 上仿真**。
   `lib/` 和 `ctrl/` 不依赖 HAL，用 gcc 编一个 `sim_main.c`，把车体运动学（两轮差速 + 积分）、8 路量化、IMU 噪声、60 ms 视觉延迟都建模，就能在**上车之前**把 PID 参数整到七八成。
   这是省试车时间最狠的一招，强烈建议。
3. 等 P4 通了再回来改 `ctrl_fsm.c` 的阈值——那些数只能在路上量出来。

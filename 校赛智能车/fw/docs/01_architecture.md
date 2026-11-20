# 01 技术栈与软件架构（方案论证稿）

版本：v0.1 / 2026-11-15
依据：`pin_plan.md`（2026-10-27 版）、`schematic_checklist.md`、赛道图 `O1CN01IYVlK4SkK1D0dH6E_...png`

---

## 0 一句话结论

**HAL + FreeRTOS 这个方向是对的，但要把分工倒过来：FreeRTOS 只负责"调度"，实时性交给 TIM6 硬件节拍 + 任务通知。同时三处要改：**

1. 控制周期 **10 ms → 5 ms**（理由见 §1，量化误差和量化台阶）。
2. 转向从"单环 PD"改成 **位置外环 + 陀螺仪角速度内环** 的串级（这才是 HWT101 的最大价值）。
3. K230 **不进控制内环**，只做"事件触发 + 远距预瞄"。

F407 性能确实够用，但**这套系统的瓶颈从来不是算力**，而是延迟、带宽、确定性。选型要围绕这三件事。

---

## 1 从引脚定义反推出来的真实约束

| 资源 | 接口 | 数量级 | 对软件的含义 |
|---|---|---|---|
| 8 路循迹 | 软件 I2C，PD3/PD4，5V，100 kHz | 读 1 字节 ≈ 39 bit ≈ **0.39 ms** | 200 Hz 读取 ≈ **7.8% CPU**；提到 400 kHz → ≈100 µs，2% |
| HWT101 | USART2 @ **9600** | 11 B/帧，100 Hz → **11000 bit/s** | **9600 撑不住 100 Hz（上限约 87 Hz）→ 必须改 115200** |
| K230 | USART6 @115200 | 30 fps × ≈14 B = 420 B/s | 带宽富裕，瓶颈是**视觉处理延迟 50–100 ms** |
| HC06 | USART3 @115200 | 遥测 20–50 Hz | 必须 DMA 非阻塞发送 |
| OLED | I2C1 @400 kHz | 全屏 1 KB ≈ **25 ms** | **只能放最低优先级任务**，否则一次刷屏拖 5 个控制周期 |
| 编码器 | TIM3/TIM4 编码器模式，4 倍频 | 1.2 m/s 时 ≈ 7.8 k count/s | 5 ms 采样 ≈ **39 count**，速度量化 ≈ 2.6%；10 ms → 5.2% |
| 电机 PWM | TIM1 20 kHz，ARR=8399 | 8399 级 | 分辨率足够；瓶颈是 TB6612 1.2 A 连续能力 |
| 主控 | F407 @168 MHz + FPU | — | 上述总负载 CPU < 20% |

**推论：**
- 用 10 ms 采样，速度量化误差 5%，且低速段更差 → 5 ms 是免费的精度提升（CPU 只多 3%）。
- 1.2 m/s 时 5 ms 走 6 mm，8 路间距约 10 mm → 每周期前进约 0.6 个传感器间距，刚好够；10 ms 就是 1.2 个间距，必然出现"跨过一个传感器"的漏判。
- 所以 **5 ms 不是"更精细"，是"刚好够用"的下限**。

---

## 2 技术栈选型

| 层 | 选型 | 理由 |
|---|---|---|
| 工程生成 | STM32CubeMX 6.x，`.ioc` 入库 | 引脚/时钟/DMA 可视化，改板不用重写 |
| 外设驱动 | HAL，热点用少量 LL（如 `BSRR` 直写） | DMA/IDLE、编码器模式开箱可用，生态成熟 |
| 调度 | FreeRTOS 10.x + CMSIS-RTOS2 | 多速率 + 阻塞隔离 |
| 算法 | 纯 C99，无动态内存 | 可在 PC 上单测、做闭环仿真 |
| 工具链 | Keil MDK 5（团队熟悉）或 CMake + arm-none-eabi-gcc + VS Code | `.ioc` 能同时生成两套，别纠结 |
| 版本管理 | Git，只入库源码/`.ioc`/文档 | 编译产物、`Drivers/` 可入库也可 gitignore |
| 上位机 | VOFA+（FireWater/JustFloat）+ 自定义协议并存 | 波形调试效率比串口助手高一个量级 |
| 参数整定 | 蓝牙在线改参 + Flash 保存（最后 16 KB sector） | 避免每次调参重烧 |

### 2.1 为什么不选这些

- **寄存器 / 标准库**：TB6612 + 双编码器 + 3 路串口 DMA + 软 I2C，手写寄存器维护成本远超收益。
- **纯 LL**：省下的 ROM 用不上，反而丢掉 DMA/编码器的成熟封装。
- **裸机前后台**：能做，但 OLED 刷屏（25 ms）、I2C 超时、串口阻塞都会直接污染 5 ms 节拍；靠"关中断"绕过去，又会毁掉软 I2C 时序。用 10 KB RAM 换掉这类系统性风险，值得。
- **换 H7 / 上 RT-Thread**：本题不需要，换平台只会引入新的不确定性。

---

## 3 FreeRTOS 到底带来什么

**收益：**
1. **阻塞隔离**——OLED、Flash、蓝牙发送可以放心阻塞，控制任务不受影响。这是最实在的一条。
2. **多速率天然表达**——5 ms 控制 / 10 ms 状态机 / 20 ms 遥测 / 100 ms UI。
3. **可观测性**——栈水位、运行时统计、任务状态，调车时能少掉一半头发。

**代价（必须防）：**
1. 中断优先级配错 → 丢中断或卡死。
2. 栈溢出 → 随机 HardFault。
3. 用 1 ms tick 做控制节拍 → 5 ms 周期抖动 20%，等于白搭。

**结论：用 RTOS，但把实时性从 RTOS 手里拿回来——控制节拍走 TIM6 硬件中断 + `xTaskNotifyFromISR`，不用 `vTaskDelayUntil`。**

### 3.1 五条铁律

1. 控制任务优先级最高（用户任务里），由 TIM6 通知唤醒，**不用延时函数**。
2. ISR 里只允许 `xxxFromISR` + `portYIELD_FROM_ISR`；禁止 `HAL_Delay` / `printf` / `malloc` / 浮点长运算。
3. 所有会调 `FromISR` 的中断，NVIC 抢占优先级数值 **≥ 5**（`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5`）：TIM6 设 6，串口 DMA 设 6，SysTick/PendSV 归内核。
4. **HAL 时基改到 TIM7**（CubeMX → Timebase Source = TIM7）。FreeRTOS 占了 SysTick，HAL 再用会打架；而 TIM6 要留给 5 ms 控制节拍。**这是最容易踩的坑**：CubeMX 一开 FreeRTOS，默认就想拿 TIM6 当 HAL 时基。
5. 任务用 `xTaskCreate`（heap_4，总堆 24 KB 足够）；开 `configCHECK_FOR_STACK_OVERFLOW = 2`。要求确定性内存时可改 `xTaskCreateStatic`，但记得补 `vApplicationGetIdleTaskMemory`。

### 3.2 FreeRTOSConfig 关键项

```c
configTICK_RATE_HZ                    1000
configMAX_PRIORITIES                  8
configUSE_PREEMPTION                  1
configUSE_TIME_SLICING                0
configMAX_SYSCALL_INTERRUPT_PRIORITY  5
configCHECK_FOR_STACK_OVERFLOW        2
configUSE_MALLOC_FAILED_HOOK          1
configTOTAL_HEAP_SIZE                 (24 * 1024)
configUSE_TRACE_FACILITY              1   /* 调车期开，出成绩后关 */
```

编译：`-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2`。
用 `-O2` 不用 `-Ofast`：浮点重排会让"同一份参数两次跑出不同结果"，调参时是灾难。

---

## 4 任务划分

| 任务 | 优先级 | 唤醒源 | 周期 | 栈(字) | 职责 | WCET 目标 |
|---|---|---|---|---|---|---|
| `ctrl` | 6 | TIM6 通知 | **5 ms** | 256 | 读循迹 + 编码器、串级控制、PWM 输出、失效保护；每 2 拍跑一次状态机 | < 600 µs |
| `comm` | 3 | 自身延时 | 2 ms 轮询 / 20 ms 发送 | 384 | 三路串口收包解析、遥测组帧、命令处理 | < 300 µs |
| `ui` | 1 | 自身延时 | 100 ms | 384 | OLED、按键、LED、蜂鸣器、电压 | < 2 ms（可被抢占） |

> 状态机故意放在 `ctrl` 里（每 2 拍跑一次）：它只读传感器、只写参数，几十微秒。等元素逻辑超过 200 行再拆成独立任务，现在拆是给自己找同步问题。
>
> 8 路循迹的软 I2C 读取放在 `ctrl` 任务里而不是感知任务里，**为的是零延迟**：读到即用。代价是 100 µs（400 kHz），占 2% CPU，划算。

---

## 5 数据流

```
TIM6 (5 ms, 200 Hz) ──notify──► ctrl 任务
                                  │  ├─ 软 I2C 读 0x12（8 路）
                                  │  ├─ TIM3/TIM4 读编码器
                                  │  ├─ 串级控制 → PWM/方向/STBY
                                  │  ├─ 失效保护
                                  │  └─ seqlock 发布状态快照
                                  ▼
USART2 HWT101 ─┐                  car_state_t
USART6 K230   ─┼─ DMA(circular) ─► comm 任务(2 ms) ─► 解析 ─► seqlock 发布
USART3 HC06   ─┘                                      │
                                                      └─► 命令 ─► g_params / mode
状态快照 ─► comm（遥测 50 Hz） / ui（OLED 10 Hz）
```

---

## 6 目录结构与依赖规则

```
fw/
├─ app/   app_config.h   app_types.h   app_main.c
├─ ctrl/  ctrl_core.[ch]   ctrl_fsm.[ch]
├─ bsp/   bsp_motor  bsp_encoder  bsp_track  bsp_imu  bsp_vision  bsp_uart  bsp_ui
└─ lib/   pid  proto          ← 纯 C，不依赖 HAL
```

依赖严格单向：`app → ctrl → bsp → HAL`，`lib` 被上层引用但不反向依赖。

**硬性规定：`lib/` 和 `ctrl/` 里不出现 `stm32f4xx_hal.h`。** 这样算法层能在 PC 上编译、单测、跑闭环仿真——这是"性能够用"之外，最能提升胜率的一条工程手段。

---

## 7 CPU 预算（1.2 m/s、200 Hz 控制）

| 项 | 单次 | 频率 | 占用 |
|---|---|---|---|
| 软 I2C 读循迹（400 kHz） | 110 µs | 200 Hz | 2.2% |
| 编码器 + 速度换算 | 5 µs | 200 Hz | 0.1% |
| 串级控制（浮点） | 15 µs | 200 Hz | 0.3% |
| 元素状态机 | 20 µs | 100 Hz | 0.2% |
| 三路串口解析 | 30 µs | 500 Hz | 1.5% |
| 遥测 + CRC8 | 20 µs | 50 Hz | 0.1% |
| **OLED 全屏刷新** | **25 ms** | 5 Hz | **12.5%** ← 唯一大头 |
| 合计 | | | **≈ 17%** |

结论：F4 余量充足。真要再压，就把 OLED 改局部刷新或降到 2 Hz——**别去优化 PID**。

---

## 8 风险与对策

| 风险 | 触发场景 | 对策 |
|---|---|---|
| FreeRTOS 中断优先级配错 | 首次移植 | 先裸机验证全部外设，再套 RTOS；`app_config.h` 留 `APP_USE_RTOS` 开关可回退 |
| 软 I2C 被中断拉长低电平 | 高频 DMA | I2C 协议允许主设备拉长 SCL 低电平，安全；但要保证 SCL 高电平 ≥ 1.3 µs |
| 9600 波特率撑不住 IMU | 一上电 | 先用上位机把 HWT101 改 115200 |
| TB6612 余量不足 | 堵转/急停 | 软件斜坡 + 起步限流；备选 DRV8871/A4950 |
| 视觉延迟 80 ms | 高速入弯 | 视觉只做决策/预瞄，不做内环；加外推补偿 |
| OLED 拖慢系统 | UI 任务 | 最低优先级 + 局部刷新 + 只显示 4 行 |
| 参数丢失 | 重烧固件 | 调好的参数存 Flash 最后一页，上电校验 |

---

## 9 需要你确认的 4 件事

1. 比赛规则里 K230 是**必须识别某种标志**，还是"能识别就加分"？（决定视觉是主路径还是旁路）
2. 赛道是否**赛前固定**、允不允许用里程脚本切元素？（决定状态机的触发源）
3. 工具链到底用 Keil 还是 CubeIDE/CMake？现在有没有已经跑通的电机/串口代码可以沿用？
4. 电机实际**减速比**和**编码器线数**（决定速度上限，以及 5 ms 采样够不够分辨率）。

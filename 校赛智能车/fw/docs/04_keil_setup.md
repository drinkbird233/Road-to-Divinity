# 04 CubeMX + Keil 落地手册

版本：v0.1 / 2026-11-15
适用：STM32CubeMX 6.x + Keil MDK-ARM 5.38+（AC6 编译器）

---

## 0 分工原则

**CubeMX 只管"硬件配置"，业务代码全在 `fw/` 里，两边只靠三样东西连接：**

1. `include path` 指向 `fw/app`、`fw/ctrl`、`fw/bsp`、`fw/lib`；
2. 两个调用点：`app_init()`（USER CODE 2）、`app_start()`（USER CODE RTOS_THREADS）；
3. 一个 `#include "app_main.h"`。

这样 CubeMX 重新生成代码时，你的业务代码一行都不会动。

---

## 1 CubeMX 端：必配的 12 项

| # | 位置 | 配置 | 为什么 |
|---|---|---|---|
| 1 | Project Manager | Toolchain = **MDK-ARM V5**；Application Structure = Advanced | 生成 .uvprojx |
| 2 | RCC | HSE = Crystal/Ceramic Resonator，8 MHz | 板载晶振 |
| 3 | Clock | SYSCLK 168 / AHB 168 / APB1 42 / APB2 84 | TIM3/4 = 84 MHz，TIM1 = 168 MHz |
| 4 | SYS → Timebase Source | **TIM7** | FreeRTOS 占 SysTick；TIM6 要留给 5 ms 控制节拍 |
| 5 | TIM1 | CH1/CH2 PWM，PSC=0，ARR=8399 → 20 kHz | 电机 |
| 6 | TIM3 / TIM4 | Encoder Mode **TI1 and TI2**，PSC=0，ARR=65535 | 4 倍频，对应 `CFG_ENCODER_MULT=4` |
| 7 | TIM6 | PSC=8399，ARR=**49**，勾 update interrupt | 84 MHz/8400/50 = **200 Hz = 5 ms** |
| 8 | USART1/2/3/6 | 115200 8N1 | 注意 **HWT101 要先改成 115200** |
| 9 | USART2/3/6 → DMA RX | Mode = **Circular**，Memory 自增，Peripheral 不自增 | `bsp_uart.c` 靠 NDTR 差值取数，非环形会停住 |
| 10 | NVIC | Priority Group = **4 bits**；TIM6 抢占 **6**；DMA/USART 抢占 **6** | 调用 `FromISR` 的中断必须数值 ≥ 5 |
| 11 | FREERTOS | CMSIS_V2，TICK_RATE **1000**，TOTAL_HEAP **24576**，MAX_PRIORITIES **8**，USE_PREEMPTION = Enabled，USE_TIME_SLICING = **Disabled** | 见 `docs/01` §3.2 |
| 12 | GPIO | 按 `README.md` §2.2 打 User Label | 代码直接引用 `XXX_Pin` |

---

## 2 第一个坑：CubeMX 的 defaultTask 会压过你的控制任务

CubeMX 一旦启用 FreeRTOS，会自动建一个 `defaultTask`，优先级 **`osPriorityNormal` = 24**。
而我们的控制任务用的是**原生 FreeRTOS 优先级 5**（数值越大越高）。

**结果：控制任务永远抢不到 CPU，车一动不动，而且没有任何报错。** 这个坑能查一下午。

**处理（二选一）：**

- ✅ 推荐：CubeMX → FreeRTOS → **Tasks and Queues** → 把 `defaultTask` 删掉（我们自己在 `app_start()` 里建任务）。
- 或者：把 `defaultTask` 优先级改成 `osPriorityIdle`，并在 `StartDefaultTask` 里第一行 `vTaskSuspend(NULL);`。

---

## 3 Keil 端：工程设置

### 3.1 Options for Target

| 页 | 设置 | 说明 |
|---|---|---|
| Target | Xtal 8 MHz；**Floating Point Hardware = Single Precision** | F407 有 FPU，不选就全是软浮点，慢 5–10 倍 |
| Target | Use MicroLIB —— **看情况**（见 §3.3） | |
| C/C++ | Optimization **-O2**（不要 -Ofast） | 浮点重排会让同样参数跑出不同结果 |
| C/C++ | Define：`USE_HAL_DRIVER, STM32F407xx` | CubeMX 自动加的，别删 |
| C/C++ | Include Paths 见下表 | |
| Linker | 勾 Use Memory Layout from Target Dialog | 默认即可 |
| Debug | ST-Link Debugger → Settings → Flash Download → 勾 **Reset and Run** | 下载后自动跑 |

### 3.2 Include Paths（4 条）

```
..\Core\Inc
..\Drivers\STM32F4xx_HAL_Driver\Inc
..\Drivers\CMSIS\Device\ST\STM32F4xx\Include
..\Drivers\CMSIS\Include
```

再补上我们自己的四层（按实际目录调整）：

```
..\fw\app
..\fw\ctrl
..\fw\bsp
..\fw\lib
```

> 如果你把 `fw/` 放在工程目录外面（比如 `..\..\fw`），路径就用相对或绝对，Keil 都认。

### 3.3 MicroLIB 到底勾不勾

- **勾**：省 8–10 KB Flash，启动快。但 **ARM 的 MicroLIB `printf` 不支持 `%f`**，浮点会打成空白或乱码。
- **不勾**：`printf("%.2f")` 正常，代价是代码变大。

**建议：调试期不勾**（要看 PID 波形和误差就得打浮点），出成绩前再考虑。要省空间就自己写定点打印，别折腾 MicroLIB。

### 3.4 把 fw 加进工程

1. 左侧 Project 窗口 → 右键 Target → **Manage Project Items**；
2. 建 4 个 Group：`app` / `ctrl` / `bsp` / `lib`；
3. 每组 → Add Files → 选中对应的 `.c`（**每组只加 .c，头文件不用加**）；
4. 编译。

---

## 4 ⚠️ CubeMX 重新生成会覆盖 `.uvprojx`

这是 Keil + CubeMX 组合最烦的一点：**你在 Keil 里手动加的 4 个 Group，CubeMX 一按 "Generate Code" 就全没了。**

三种应对，按省心程度排序：

1. **一次性生成，之后不再重新生成**。改引脚就在 Keil 里手改 `main.c` 的 `MX_GPIO_Init`。
2. **每次生成前把 `.uvprojx` 复制一份**，生成后把 `<Groups>` 段合并回去。
3. **改用 CMake**（CubeMX 也能生成 CMake），文件列表是 `CMakeLists.txt` 里的 `target_sources()`，可以放在 `fw/CMakeLists.txt` 里单独维护，永远不会丢。
   > 如果这个项目还有 2 个月，我建议直接上方案 3；如果只剩两周，用方案 1。

---

## 5 编译/链接报错对症表

| 报错 | 真正原因 | 解法 |
|---|---|---|
| `L6218E: Undefined symbol htim1` | 句柄在 `main.c` 里定义，`bsp_motor.c` 没 `extern` | 各 `bsp/*.c` 顶部已有 `extern TIM_HandleTypeDef htim1;`，检查拼写 |
| `implicit declaration of function 'app_init'` | 没 include | `main.c` 加 `#include "app_main.h"` |
| `multiply defined HAL_TIM_PeriodElapsedCallback` | CubeMX 的 `stm32f4xx_hal_timebase_tim.c` 也定义了一个 | **删掉那个文件里的定义**，用 `app_main.c` 的 |
| `L6915E: Library reports error: __use_no_semihosting` | 用标准库但没实现 `_sys_exit` 等 | 勾 MicroLIB，或补 4 个空桩函数 |
| `Undefined symbol TRACK_SCL_Pin` | CubeMX 里没打 User Label | 按 `README.md` §2.2 补标签 |
| 编译通过，但车不动、串口无输出 | **TIM6 的 NVIC 没开** 或 defaultTask 抢占了控制任务 | 查 §1 第 10 项、§2 |
| 一上电就 HardFault | 任务栈太小 / `heap` 不够 | 堆 ≥ 24 KB；`configCHECK_FOR_STACK_OVERFLOW = 2` |
| `printf` 打出空白 | 勾了 MicroLIB 又用 `%f` | 见 §3.3 |

---

## 6 上电前的最后三件事

1. **别接电机**，先只接调试串口，确认 `app_log("[car] boot ok")` 能打出来。
2. 用 `bsp_track_probe(0x12)` 和 I2C 扫描确认 OLED(0x3C)、循迹(0x12) 都在。
3. 手转车轮，看遥测里的 `speed_l/speed_r` 符号对不对、量级对不对（转一圈应该是 204 mm）。

---

## 7 Keil 下看波形

- `printf` 走 USART1（115200），接 USB-TTL。
- 想看波形：把遥测帧直接用 **VOFA+ 的 FireWater 协议**发（`数据1,数据2,...\n`），比自己解析 0xAA55 帧快得多。
  建议在 `comm_task` 里加一个编译开关 `CFG_USE_VOFA 1`，调试期开、比赛关。
- Keil 的 Logic Analyzer（仿真）只能看变量，实测意义不大；**真实波形才是调参依据**。

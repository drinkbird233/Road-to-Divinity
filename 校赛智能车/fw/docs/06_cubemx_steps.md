# 06 CubeMX 配置实战（逐步进行中）

芯片：STM32F407ZGT6（核心板 LXBF407ZG-P1 V2.0，LQFP144）
工具：STM32CubeMX 6.x + Keil MDK-ARM 5.38+
原则：**每步做完先验证，通过了再进下一步。** 一次改太多东西，出问题无法定位。

进度总览：

| 步骤 | 内容 | 状态 |
|---|---|---|
| S0 | 环境自检（CubeMX 装 F4 包、Keil 能编译空工程） | ✅ |
| S1 | 新建工程 + RCC/SYS + 时钟树 168 MHz + Project Manager | ✅ |
| S2 | 纯 GPIO 14 个引脚 + User Label（本步） | 🔄 |
| S3 | TIM1 PWM / TIM3+TIM4 编码器 / TIM6 节拍 | ⬜ |
| S4 | 四路串口 + DMA Circular + NVIC 优先级 | ⬜ |
| S5 | I2C1 / ADC1 | ⬜ |
| S6 | FreeRTOS（CMSIS_V2、堆、删 defaultTask） | ⬜ |
| S7 | 生成代码 + 并入 fw/ + Keil 工程设置 | ⬜ |
| S8 | 编译下载 + 上电自检（串口打印） | ⬜ |

⚠️ **不要给 CubeMX 生成 `.ioc` 文件**（网上或 AI 生成的都别用）。
`.ioc` 里带版本号和几百个内部键，版本对不上就打不开，反而浪费时间。手点一遍 20 分钟，比修文件快。

---

## S0 环境自检

**做什么**

1. CubeMX → `Help` → `Manage embedded software packages` → 确认 **STM32F4** 系列已安装（F4 的包，不是 F1）。
2. Keil 里随便建个空工程或打开任意 `.uvprojx`，能 `Build`（0 error）即可，**先不下载**。
3. 确认 ST-Link 驱动装好（设备管理器里能看到 STLink）。

**判据**

- [ ] CubeMX 里能搜到 `STM32F407ZGT6`
- [ ] Keil 能编译（哪怕是个空 main）
- [ ] 插上板子，Keil `Options for Target → Debug → Settings` 能识别到 ST-Link 和芯片 IDCODE

---

## S1 新建工程 + 时钟树（本步只做这些，一个外设都别开）

### 1.1 新建

```
File → New Project → 搜索框输入 STM32F407ZGT6
→ 列表里选 STM32F407ZGTx（ZGT6 属于 ZGTx 系列）
→ 双击 → 进入 Pinout & Configuration
```

> 型号一定确认是 **ZGT6 / LQFP144 / 1 MB Flash**。选成 ZET6（512 KB）后面会莫名其妙链接超限。

### 1.2 先配 System Core（顺序不能反）

**先配 RCC，时钟树才允许填 HSE 频率。**

| 位置 | 设置 | 说明 |
|---|---|---|
| `System Core → RCC → HSE` | **Crystal/Ceramic Resonator** | 板载 8 MHz 晶振 |
| `System Core → RCC → LSE` | **Disable** | 板载 32.768 kHz 我们不用，少一个变量 |
| `System Core → SYS → Debug` | **Serial Wire** | ⚠️ 不选这个，SWD 引脚会被当普通 GPIO，以后**再也连不上** |
| `System Core → SYS → Timebase Source` | **TIM7** | ⚠️ 必须提前设。FreeRTOS 占 SysTick，TIM6 要留给 5 ms 控制节拍 |

### 1.3 时钟树（Clock Configuration 页）

照着填，填完 CubeMX 会自动算：

| 项 | 值 | 结果 |
|---|---|---|
| Input frequency | `8` MHz | HSE |
| PLL Source Mux | **HSE** | |
| PLLM (/M) | `8` | → 1 MHz 比较频率 |
| PLLN (×N) | `336` | → 336 MHz VCO |
| PLLP (/P) | `2` | → **168 MHz SYSCLK** |
| PLLQ (/Q) | `7` | → 48 MHz（USB/SDIO 用，本项目不用但留正确值） |
| System Clock Mux | **PLLCLK** | |
| AHB Prescaler | `/1` | → HCLK **168 MHz** |
| APB1 Prescaler | `/4` | → 42 MHz；**TIM3/4/6/7 = 84 MHz** |
| APB2 Prescaler | `/2` | → 84 MHz；**TIM1 = 168 MHz** |

**判据（必须逐条对上，差一个数都不行）**

- [ ] HCLK (MHz) = **168**
- [ ] PCLK1 = **42**，PCLK2 = **84**
- [ ] APB1 Timer clocks = **84**
- [ ] APB2 Timer clocks = **168**
- [ ] 时钟树右下角无红色警告

> 这三个数直接决定后面所有计算：
> PWM 20 kHz = 168 MHz / 8400；TIM6 200 Hz = 84 MHz / 8400 / 50；编码器计数率 = 84 MHz 上限。

### 1.4 Project Manager 页

| 项 | 值 | 说明 |
|---|---|---|
| Project Name | `SmartCar_F407` | **英文、无空格** |
| Project Location | 你自己的工作目录（如 `D:\默认存储\共享记忆\校赛智能车\firmware`） | 路径里**不要有中文和空格**，Keil 会出各种怪问题 |
| Application Structure | **Advanced** | 外设初始化分文件，好找 |
| Toolchain / IDE | **MDK-ARM**，Min Version `V5.32` | |
| `Code Generator` 页 | 勾 **Generate peripheral initialization as a pair of .c/.h files per peripheral** | 每个外设一个文件，USER CODE 位置清晰 |
| `Code Generator` 页 | 勾 **Keep User Code when re-generating**（默认已勾） | 保住你的代码 |
| `Code Generator` 页 | `Copy only the necessary library files`（可选） | 工程小一点 |

### 1.5 生成并验证工具链

```
Project → Generate Code → Open Project
```

**判据**

- [ ] Keil 自动打开，`Build`（F7）→ **0 Error(s)**
- [ ] 下载（F8）→ 板子复位后能停在 `main()` 的 `while (1)` 里（Keil 调试模式验证）
- [ ] 目视检查 `Core/Inc/main.h` 里能看到 `TIM7` 相关定义、`SystemClock_Config()` 里 HSE 已启用

**本步不要做的事**

- ❌ 不要开 FreeRTOS（会改 Timebase Source，把 TIM7 冲掉）
- ❌ 不要配任何 GPIO（等 S2 的统一引脚表，一次配完，避免漏）
- ❌ 不要开串口（等 S4，DMA 要一起配）

---

## S2 引脚分配：纯 GPIO 14 个

**本步只配"必须手点 Mode"的 14 个引脚。** 定时器、串口、I2C、ADC 的引脚由外设在 S3–S5 自动占用，那时再打标签。

### 操作手法

1. 在 Pinout 视图里**左键点引脚** → 弹出菜单 → 选 `GPIO_Output` / `GPIO_Input`；
2. 选中引脚后，左侧 `System Core → GPIO` 面板里设 `GPIO mode / Output level / Pull-up / Speed`；
3. 给引脚命名：**右键引脚 → `Enter User Label`**（比在面板里打字快）。

### 逐引脚表（照抄）

| # | 引脚 | 功能 | GPIO mode | 输出电平 | 上下拉 | 速度 | **User Label** |
|---|---|---|---|---|---|---|---|
| 1 | PC0 | 蜂鸣器 | Output Push Pull | **Low** | No pull | Low | `BUZZER` |
| 2 | PC1 | KEY1 | Input | — | **Pull-up** | — | `KEY1` |
| 3 | PC2 | KEY2 | Input | — | **Pull-up** | — | `KEY2` |
| 4 | PC3 | KEY3 | Input | — | **Pull-up** | — | `KEY3` |
| 5 | PC4 | LED1 | Output Push Pull | **Low** | No pull | Low | `LED1` |
| 6 | PC5 | LED2 | Output Push Pull | **Low** | No pull | Low | `LED2` |
| 7 | PE8 | 左电机 IN1 | Output Push Pull | **Low** | No pull | Medium | `MOTOR_L_IN1` |
| 8 | PE10 | 左电机 IN2 | Output Push Pull | **Low** | No pull | Medium | `MOTOR_L_IN2` |
| 9 | PE12 | 右电机 IN1 | Output Push Pull | **Low** | No pull | Medium | `MOTOR_R_IN1` |
| 10 | PE13 | 右电机 IN2 | Output Push Pull | **Low** | No pull | Medium | `MOTOR_R_IN2` |
| 11 | PE14 | TB6612 使能 | Output Push Pull | **Low** | No pull | Medium | `MOTOR_STBY` |
| 12 | PD3 | 软 I2C SCL | **Output Open Drain** | **High** | No pull | **Very High** | `TRACK_SCL` |
| 13 | PD4 | 软 I2C SDA | **Output Open Drain** | **High** | No pull | **Very High** | `TRACK_SDA` |
| 14 | PE15 | HC06 连接状态 | Input | — | **Pull-down** | — | `BT_STATE` |

### 四个必须讲究的点（错了很难查）

1. **PD3/PD4 必须 `Open Drain` + 初始 `High`。**
   外部 4.7k 上拉到 5V，配成推挽会把 5V 硬拉进 MCU；初始低电平会被模块当成一个 START 条件。
   速度选 `Very High` 是为了软 I2C 跑 400 kHz 时边沿够快。

2. **PE14 `MOTOR_STBY` 初始 `Low`。**
   TB6612 上电必须断输出。这行是"车不动"的第一道保险，不要改。

3. **PE8/PE10/PE12/PE13 全部初始 `Low`。**
   板上已有 10k 下拉，软件再给一次双重保险。四路方向脚在上电瞬间必须是低。

4. **PE15 `BT_STATE` 选 `Pull-down`。**
   蓝牙没连时读 0，连上才 1；下拉保证悬空不会误判成"已连接"。

### 本步绝对不要碰的引脚

| 引脚 | 原因 |
|---|---|
| PC8–PC12、PD2 | 板载 MicroSD（SDIO） |
| PB3、PB5、PD11–PD13 | 板载 FPC LCD |
| PA15、PE6 | 板载按键 |
| PC13 | 板载 LED |
| PA11、PA12 | Type-C USB |
| PA13、PA14 | SWD（已由 SYS Debug 占用，别动） |
| PH0、PH1 | 8 MHz 晶振 |
| PC14、PC15 | 32.768 kHz（S1 里已 Disable，保持未配置） |
| PB2 | BOOT1 |
| PA9/PA10、PA2/PA3、PB10/PB11、PC6/PC7 | 留给串口（S4） |
| PB8/PB9、PA4 | 留给 I2C1 / ADC（S5） |
| PE9、PE11 | 留给 TIM1 PWM（S3） |

### 判据

- [ ] Pinout 视图里这 **14 个引脚全部变绿**（不是黄色）
- [ ] 鼠标悬停每个引脚，浮出的名字和上表 **一字不差**
- [ ] `Generate Code` 后在 `Core/Inc/main.h` 里能搜到 14 组宏，例如：
  ```c
  #define MOTOR_STBY_Pin        GPIO_PIN_14
  #define MOTOR_STBY_GPIO_Port  GPIOE
  ```
- [ ] 用编辑器搜 `_GPIO_Port`，应该有 **14 行**（多一行说明多配了，少一行说明漏了）
- [ ] Keil 编译仍然 0 Error

> 提示：名字必须**逐字**一致（大小写、下划线）。`bsp_*.c` 里是直接 `#include "main.h"` 引用这些宏的，差一个字母就编译报 `Undefined symbol`。

---

## S2.5 首次生成后的代码审计（2026-11-16）

对 `D:\car\smartcar\SmartCar_F407` 逐文件核对的结果。

### ✅ 通过项

| 检查点 | 证据 | 结论 |
|---|---|---|
| 芯片型号 | `SmartCar_F407.ioc: Mcu.UserName=STM32F407ZGTx` | ✅ 1 MB Flash，正确 |
| HAL 时基 | `Mcu.Pin18=VP_SYS_VS_tim7`，`Core/Src/stm32f4xx_hal_timebase_tim.c` 里配的是 `htim7` | ✅ 用 TIM7，正确 |
| SWD | `PA13/PA14` 已占用 | ✅ 调试口保住了 |
| 分频 | `APB1=DIV4(42M, TIM 84M)`，`APB2=DIV2(84M, TIM 168M)` | ✅ 与文档一致 |
| SYSCLK | `HCLK=168 MHz`，`FLASH_LATENCY_5` | ✅ 168 MHz 对应 5 等待周期，正确 |
| 14 个 GPIO 模式 | `gpio.c`：PP×5(电机) / PP×3(蜂鸣器+LED) / PU×3(按键) / PD×1(BT) / OD×2(软 I2C) | ✅ 全部正确 |
| 14 个 User Label | `main.h` 里 14 组 `_Pin`+`_GPIO_Port`，逐字一致 | ✅ 全部正确 |
| 电机上电安全 | `gpio.c` 先把 IN1/IN2/STBY 全写 `RESET` 再初始化 | ✅ 上电不动 |
| 工程结构 | Advanced，`gpio.c` 独立 | ✅ 符合预期 |
| 编译 | `0 Error(s), 0 Warning(s)`，`Code=4242` | ✅ 干净 |

### ❌ 必修 2 项

**① `TRACK_SDA` 初始电平是 Low（唯一一处 GPIO 错）**

`Core/Src/gpio.c:62-65`：
```c
HAL_GPIO_WritePin(TRACK_SCL_GPIO_Port, TRACK_SCL_Pin, GPIO_PIN_SET);    /* ✓ High */
HAL_GPIO_WritePin(TRACK_SDA_GPIO_Port, TRACK_SDA_Pin, GPIO_PIN_RESET);  /* ✗ 应为 SET */
```

SCL 已被拉高、SDA 却拉低 —— 这在上电瞬间**正好构成一个 I2C START 条件**。
从机可能因此进入接收态，把之后第一次真实通信的时序错开一位，表现为"偶尔读不到 0x12"或"数据错位一帧"。

→ CubeMX：点 `PD4` → GPIO 面板 `GPIO output level` 改 **High** → 重新生成。

**② 时钟源是 HSI，8 MHz 晶振根本没启用**

`Core/Src/main.c:121-126`：
```c
RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
RCC_OscInitStruct.HSIState = RCC_HSI_ON;
RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
RCC_OscInitStruct.PLL.PLLM = 8;    /* 16 MHz / 8 = 2 MHz */
RCC_OscInitStruct.PLL.PLLN = 168;  /* → 336 MHz VCO */
```

现在跑的是 `16 MHz HSI ÷ 8 × 168 ÷ 2 = 168 MHz`，**能跑，但用的是内部 RC**：

- HSI 精度 ±1%（25 ℃）、全温区可达 ±4%；三路 115200 串口工作在误差边缘；
- 板上有 8 MHz 晶振却没用上；
- `.ioc` 里 `RCC.HSE_VALUE=25000000` 与实物 **8 MHz 不符** —— 现在无害（没启用 HSE），
  但一旦切到 HSE 而忘了改这个值，**串口波特率会错 3 倍，全是乱码**。

→ Clock Configuration：`PLL Source Mux` 选 **HSE**，HSE 输入频率填 **8**。
CubeMX 会重算为 `M=8, N=336, P=2 → 168 MHz`，并把 `HSE_VALUE` 改成 8000000。

**③ 建议顺手改（可选）**：`PLLQ=4 → 84 MHz`，USB 规范值是 48 MHz（`PLLQ=7`）。本项目不用 USB，不改也能跑，但改成规范值不会错。

### 📌 给 S7 记一笔

`Core/Src/main.c:162` 已经有：
```c
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM7) { HAL_IncTick(); }
}
```

**所以 `app_main.c` 里不能再定义同名函数**（会 `multiply defined`）。
S7 的接入方式改为：把 TIM6 的通知挂到 main.c 的 `/* USER CODE BEGIN Callback 0 */` 段里。
届时我会把 `app_main.c` 的对应实现改掉，不让你手动维护。

---

## S3 定时器：TIM1 PWM / TIM3+TIM4 编码器 / TIM6 节拍

本步的风险**不在参数，在引脚**。STM32 的同一个定时器通道几乎都有 2–4 个可选引脚，
CubeMX 默认挑"第一个没被占用的"，很容易落到 PA8/PA9/PA10 上——而 PA9/PA10 是我们的调试串口。

**规则：每配完一个定时器，立刻回 Pinout 视图确认它落在哪个脚上。**

### 3.1 TIM1 —— 20 kHz 电机 PWM

`Timers → TIM1`：

| 项 | 值 | 计算依据 |
|---|---|---|
| Clock Source | **Internal Clock** | |
| Channel1 | **PWM Generation CH1** | |
| Channel2 | **PWM Generation CH2** | |
| Prescaler (PSC) | **0** | |
| Counter Mode | Up | |
| **Counter Period (ARR)** | **8399** | 168 MHz ÷ 8400 = **20 kHz** |
| Internal Clock Division | No Division | |
| auto-reload preload | Enable | |

通道参数（CH1/CH2 各一份）：

| 项 | 值 |
|---|---|
| Mode | **PWM mode 1** |
| Pulse | **0**（上电占空比 0） |
| Output compare preload | Enable |
| Fast Mode | Disable |
| CH Polarity | High |

**引脚必须是 PE9 / PE11：**

| 通道 | 允许的脚 | 默认可能给的坑脚 |
|---|---|---|
| TIM1_CH1 | **PE9** | PA8 |
| TIM1_CH2 | **PE11** | **PA9**（调试串口 TX！） |

**操作手法**：先在 Pinout 视图里确定 PE9/PE11 还空着，然后**直接在 PE9 上左键 → 选 `TIM1_CH1`**。
如果 PA8/PA9 已经被占，先把它们 `Reset_State` 掉。

> ⚠️ **只开 CH1 和 CH2。** 一旦开了 CH3/CH4，TIM1_CH3 可能抢走 PE13（= `MOTOR_R_IN2`），
> CH4 可能抢走 PE14（= `MOTOR_STBY`）或 PA10。**这两个脚绝不能丢。**

Label：PE9 = `MOTOR_L_PWM`，PE11 = `MOTOR_R_PWM`

### 3.2 TIM3 / TIM4 —— 编码器接口

`Timers → TIM3 → Combined Channels` 选 **Encoder Mode**，TIM4 同理。

| 项 | TIM3（右） | TIM4（左） |
|---|---|---|
| Encoder Mode | **Encoder Mode TI1 and TI2** | 同左 |
| Prescaler | 0 | 0 |
| Counter Period (ARR) | **65535** | **65535** |
| CH1/CH2 Polarity | Rising Edge | Rising Edge |
| CH1/CH2 Input Filter | 0（噪声大可加到 6） | 同左 |
| 引脚 | **PA6 / PA7** | **PB6 / PB7** |
| Label | `ENC_R_A` / `ENC_R_B` | `ENC_L_A` / `ENC_L_B` |

**为什么要避开的坑脚**：

- TIM3 还能落在 PB4/PB5（PB5 是板载 LCD！）和 **PC6/PC7**（那是 K230 串口）；
- TIM4 还能落在 **PD12/PD13**（板载 LCD！）。

`Encoder Mode TI1 and TI2` = 4 倍频，和 `app_config.h` 里 `CFG_ENCODER_MULT = 4` 对应。
如果将来改成 TI1 only，记得同步改成 2。

### 3.3 TIM6 —— 5 ms 控制节拍

`Timers → TIM6` → 勾 **Activated**：

| 项 | 值 | 计算依据 |
|---|---|---|
| Prescaler | **8399** | 84 MHz ÷ 8400 = 10 kHz |
| Counter Period (ARR) | **49** | 10 kHz ÷ 50 = **200 Hz = 5 ms** |

`NVIC Settings` 页：

| 中断 | 勾选 | Preemption Priority | Sub Priority |
|---|---|---|---|
| `TIM6_DAC global interrupt` | ✅ | **6** | 0 |

> 优先级 6 是因为后面 FreeRTOS 要求"会调用 `FromISR` 的中断，优先级数值 ≥ 5"。
> 先在 CubeMX 里设好，S6 开 RTOS 时就不用回来改。
> （TIM7 是 HAL 时基，由 `HAL_InitTick` 自己设成最低优先级，不用管。）

### 判据

1. **Pinout 视图**：PE9、PE11、PA6、PA7、PB6、PB7 六个脚变绿，且悬停显示的名字与上表一致；
   **PA8/PA9/PA10/PC6/PC7/PE13/PE14 必须还是原样**（前三个不能被 TIM1 抢，PC6/7 留给 K230，PE13/14 是电机）。
2. `Core/Inc/main.h` 新增 6 组宏：`MOTOR_L_PWM`、`MOTOR_R_PWM`、`ENC_R_A`、`ENC_R_B`、`ENC_L_A`、`ENC_L_B`。
3. `Core/Src/tim.c` 里逐条对：
   ```c
   htim1.Init.Period   = 8399;                          /* = ARR，填多少就是多少 */
   htim3.Init.Period   = 65535;
   htim3.Init.EncoderMode = TIM_ENCODERMODE_TI12;
   htim6.Init.Prescaler = 8399;
   htim6.Init.Period    = 49;
   ```
   > CubeMX 的 `Counter Period (ARR)` 就是**直接填 ARR 的值**，它不会帮你 −1。
   > 20 kHz 的 ARR 已经是减过的：168 000 000 ÷ 20 000 − 1 = **8399**。
   > 5 ms 的 ARR：84 000 000 ÷ 8400 ÷ 200 − 1 = **49**。
4. `Core/Src/stm32f4xx_it.c` 里出现 `TIM6_DAC_IRQHandler`。
5. Keil 重新编译 **0 Error**。

### 本步不要做

- ❌ 不要 `HAL_TIM_Base_Start_IT(&htim6)`（那是 S8 的事，`app_main.c` 里会做）
- ❌ 不要开 TIM1 的 CH1N/CH2N（互补输出，我们用不到，开了会多占 PE8/PE10）
- ❌ 不要动 TIM7（HAL 时基，CubeMX 自己管）

### S3 审计结果（2026-11-16）

引脚全对，但**三个参数是 CubeMX 默认值没改**——这类问题编译不报错、烧进去才暴露，最容易误判成"硬件坏"。

| # | 位置 | 实际值 | 应为 | 不改的后果 |
|---|---|---|---|---|
| 1 | `tim.c:210` `htim6.Init.Period` | **65535** | **49** | 84M/8400/65536 = 0.15 Hz ≈ **6.5 秒一拍**，控制环形同虚设 |
| 2 | `tim.c:126 / 170` `EncoderMode` | **TIM_ENCODERMODE_TI1** | **TIM_ENCODERMODE_TI12** | 2 倍频 → **速度读数只有真值一半**，里程/盲走预算全错 2 倍 |
| 3 | `tim.c:251` `HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 0, 0)` | **0** | **6** | 优先级 0 是最高级；S6 开 RTOS 后 ISR 里调 `FromISR` 会 `configASSERT` 卡死 |

另外 `MOTOR_L_PWM` / `MOTOR_R_PWM` 两个 User Label 漏打（引脚本身对，代码用 `__HAL_TIM_SET_COMPARE` 不依赖这两个宏，属一致性问题）。

> **通用教训**：CubeMX 生成后"0 Error 但行为不对"，第一嫌疑永远是**没改的默认值**。
> 定时器类默认值：ARR=65535、PSC=0、EncoderMode=TI1、NVIC 优先级=0。

---

## S4 串口与 DMA

本步是**整个通信层的地基**。`bsp_uart.c` 的实现依赖两条硬约束：

1. **RX 的 DMA 必须是 Circular 模式**（`HAL_UART_Receive_DMA` 只启动一次，永远不重启）；
2. **TX 也要 DMA**（`HAL_UART_Transmit_DMA` 非阻塞发送，否则遥测会拖慢控制环）。

### 4.1 四路串口基础参数（全部相同）

| 项 | 值 |
|---|---|
| Mode | **Asynchronous** |
| Baud Rate | **115200** Bits/s |
| Word Length | **8 Bits**（含 Parity） |
| Parity | **None** |
| Stop Bits | **1** |
| Data Direction | Receive and Transmit |
| Over Sampling | **16** |
| Hardware Flow Control | **Disable**（四路全关） |

### 4.2 逐路分配

| 串口 | TX | RX | 用途 | RX DMA | TX DMA | 标签 |
|---|---|---|---|---|---|---|
| **USART1** | PA9 | PA10 | 调试 printf | 可选，建议开 | 可选 | `DBG_TX` / `DBG_RX` |
| **USART2** | PA2 | PA3 | HWT101 | **必须** | 可选 | `IMU_TX` / `IMU_RX` |
| **USART3** | PB10 | PB11 | HC06 蓝牙 | **必须** | **必须** | `BT_TX` / `BT_RX` |
| **USART6** | PC6 | PC7 | K230 视觉 | **必须** | **必须** | `K230_TX` / `K230_RX` |

> 四路都用 115200。**HWT101 出厂是 9600，必须在模块侧先改成 115200**，否则 100 Hz 数据传不完
> （11 B × 100 Hz × 10 bit = 11000 bit/s > 9600）。

### 4.3 DMA 配置（每路两份）

`Connectivity → USARTx → DMA Settings → Add`：

| 请求 | Direction | Priority | **Mode** | Increment Address | Data Width |
|---|---|---|---|---|---|
| `USARTx_RX` | Peripheral To Memory | **High** | **Circular** | Memory ✅ | Byte / Byte |
| `USARTx_TX` | Memory To Peripheral | Low | **Normal** | Memory ✅ | Byte / Byte |

**Stream 号由 CubeMX 自动分配，不用管**——`bsp_uart.c` 只用 `h->hdmarx->Instance->NDTR` 读剩余计数，与具体 Stream 无关。

> ⚠️ 如果 CubeMX 提示 Stream 冲突（USART1_RX 和 USART6_RX 都想用 DMA2 Stream2），
> 在 DMA Settings 里把其中一个的 Stream 换成备选（USART1_RX 可选 Stream 2/5；USART6_RX 可选 Stream 1/2）。
> **RX 全部用 Circular，这条不能妥协。**

### 4.4 NVIC 优先级

`System Core → NVIC` 总表里（不是外设自己的页），按下表设 **Preemption Priority**：

| 中断 | 抢占优先级 | 说明 |
|---|---|---|
| `DMA1 Stream1` ~ `DMA2 Stream7`（RX 那些） | **6** | 收数据要快 |
| `DMA* Stream*`（TX 那些） | **7** | 发数据可以慢 |
| `USART1 global` | **7** | 调试口，最不着急 |
| `USART2/3/6 global` | **6** | 错误标志检测（ORE 等） |
| `TIM6_DAC` | **6** | 已在 S3 定，这里别动 |

> 全部 ≥ 5 是硬要求：S6 开 FreeRTOS 后，数值 < 5 的中断不能调任何 `FromISR` API。
> 唯一例外是 `SysTick`(15) 和 `TIM7`(15)，那是时基，CubeMX 自己管。

### 4.5 本步的两个坑

1. **USART1 会想占 PA9/PA10，而 PA9 是 TIM1_CH2 的备选脚。**
   我们在 S3 已经把 TIM1 锁死在 PE9/PE11，所以 PA9/PA10 是空的。
   如果 CubeMX 报冲突，说明 TIM1 又跑回 PA8/PA9 了，回去检查 S3。

2. **PC6/PC7 同时是 TIM3_CH1/CH2 和 USART6 的备选脚。**
   TIM3 已锁死在 PA6/PA7，所以这两个脚可以让给 USART6。

### 判据

1. **Pinout 视图**：PA9/PA10/PA2/PA3/PB10/PB11/PC6/PC7 八个脚变绿，名字与上表一致。
2. `Core/Inc/main.h` 新增 **8 组**宏（`DBG_TX` … `K230_RX`）。
3. `Core/Src/usart.c` 里逐条对：
   ```c
   huart1.Init.BaudRate = 115200;   /* 四路都是 */
   hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;   /* RX 必须 CIRCULAR */
   hdma_usart3_tx.Init.Mode = DMA_NORMAL;     /* TX 是 NORMAL */
   ```
   用编辑器搜 `DMA_CIRCULAR`，应该正好有 **4 处**（USART1/2/3/6 的 RX）。
4. `Core/Src/stm32f4xx_it.c` 里出现 `DMA1_Stream1_IRQHandler` 等多条 DMA 中断函数。
5. Keil 编译 **0 Error**。

### 本步不要做

- ❌ 不要在 CubeMX 里勾 Interrupt 模式的 UART（我们要的是 DMA）
- ❌ 不要给 RX 选 Normal 模式——哪怕只是 USART1
- ❌ 不要开硬件流控（没接 RTS/CTS 线）

---

## S5 I2C1（OLED）+ ADC1（电池电压）

本步很短，两个外设都**不参与控制环**，出问题也不会让车跑飞。

### 5.1 I2C1 —— 只给 OLED 用

`Connectivity → I2C1 → I2C`：

| 项 | 值 | 说明 |
|---|---|---|
| I2C Speed Mode | **Fast Mode** | |
| **I2C Clock Speed** | **400000** Hz | OLED 全屏 1 KB 约 25 ms；100 kHz 会到 100 ms |
| Fast Mode Duty Cycle | **Tlow/Thigh = 2** | 标准快模式 |
| Addressing Mode | **7-bit** | SSD1306 是 7 位地址 0x3C |
| Dual Address | Disable | |
| General Call | Disable | |
| No-Stretch Mode | Disable | |

**引脚**：`PB8 = I2C1_SCL`、`PB9 = I2C1_SDA`，Label `OLED_SCL` / `OLED_SDA`。

> PB8/PB9 同时也是 TIM4_CH3/CH4 的备选脚，但 TIM4 只用了 CH1/CH2，**不冲突**。
> 上拉 4.7k 到 **3.3 V**（模块自带就不用加）。

**NVIC 不用开**：OLED 走阻塞式 `HAL_I2C_Master_Transmit`，且只在最低优先级 `ui` 任务里调用。

> ⚠️ 区分两条总线：**OLED = 硬件 I2C1（PB8/PB9，3.3 V）**；
> **八路循迹 = 软件 I2C（PD3/PD4，5 V 开漏）**。上拉绝不能共用。

### 5.2 ADC1 —— 电池电压

`Analog → ADC1` → 勾 **IN4**（PA4 自动变成 `ADC1_IN4`）。Label `VBAT_ADC`。

| Parameter | 值 | 理由 |
|---|---|---|
| Clock Prescaler | **PCLK2 divided by 4** → 21 MHz | ADC 上限 36 MHz；`/2` = 42 MHz **超标** |
| Resolution | **12 bits** | |
| Scan Conversion Mode | Disabled | 只测一路 |
| Continuous Conversion Mode | **Disabled** | 软件触发单次，10 Hz 读一次 |
| Data Alignment | Right | |
| Number Of Conversion | **1** | |
| External Trigger Source | **Regular Conversion launched by software** | 对应 `HAL_ADC_Start()` |
| Rank 1 → Channel | **4** | |
| Rank 1 → Sampling Time | **144 Cycles** | 见下 |

**为什么 144 周期**：分压 47k ∥ 10k ≈ 8.25 kΩ，源阻抗偏高。
144 cycles @ 21 MHz ≈ 6.9 µs，远大于 12 位所需的建立时间；换成 15/28 cycles 会读偏低。

**NVIC 不用开**：`bsp_ui_vbat_mv()` 是轮询，10 Hz × 8 次平均 ≈ 60 µs。

**电压核对**：12.6 V × 10k/(47k+10k) = **2.19 V**，在 3.3 V 内，安全。

### 判据

1. **Pinout**：PB8/PB9/PA4 变绿，PA4 显示 `ADC1_IN4`。
2. `Core/Src/i2c.c`：`hi2c1.Init.ClockSpeed = 400000`。
3. `Core/Src/adc.c`：`ADC_RESOLUTION_12B`、`ADC_CLOCK_SYNC_PCLK_DIV4`、`ADC_CHANNEL_4`、`ADC_SAMPLETIME_144CYCLES`。
4. `Core/Inc/main.h` 新增 `OLED_SCL`/`OLED_SDA`/`VBAT_ADC`（打了标签的话）。
5. Keil 编译 **0 Error**。

### 本步不要做

- ❌ 不要开 ADC 的 DMA / 连续转换（用不上，还多占一个 Stream）
- ❌ 不要把 ADC 时钟设成 `PCLK2/2`（42 MHz 超上限，读数会飘）
- ❌ 不要把 OLED 和循迹接同一条 I2C 总线（一个 3.3 V 一个 5 V）

---

## S6 FreeRTOS

本步是**整个配置里坑最多的一步**，而且失败方式都很隐蔽：不报错、不 HardFault，只是"车不动"或"跑到一半卡死"。

### 6.1 启用

`Middleware and Software Packs → FREERTOS` → `Mode`：

| 项 | 值 |
|---|---|
| Interface | **CMSIS_V2** |
| Timebase Source | 保持 **TIM7**（不要让它变回 SysTick） |

### 6.2 Config parameters 页

| 参数 | 值 | 理由 |
|---|---|---|
| **USE_PREEMPTION** | **Enabled** | 抢占式调度 |
| **USE_TIME_SLICING** | **Disabled** | 关掉同优先级轮转，减少抖动 |
| **TICK_RATE_HZ** | **1000** | 1 ms 节拍（仅用于超时/延时） |
| **MAX_PRIORITIES** | **8** | 我们的任务用 5/3/1 |
| MINIMAL_STACK_SIZE | 128（words） | |
| **TOTAL_HEAP_SIZE** | **24576** | 三任务 + idle ≈ 8 KB，留 3 倍余量 |
| **Memory Allocation** | **heap_4** | 支持碎片合并 |
| USE_MUTEXES | Enabled | CMSIS_V2 需要 |
| USE_COUNTING_SEMAPHORES | Enabled | |
| **USE_TIMERS** | **Disabled** | 我们不用软件定时器，省一个 512 words 的任务 |
| **CHECK_FOR_STACK_OVERFLOW** | **Option 2** | 栈画填充图 + 溢出立即钩子 |
| **USE_MALLOC_FAILED_HOOK** | **Enabled** | 堆不够时能停下来，而不是返回 NULL 静默失败 |
| USE_IDLE_HOOK | Disabled | |
| USE_TRACE_FACILITY | Enabled（调试期） | 能看栈水位 |
| USE_PORT_OPTIMISED_TASK_SELECTION | Enabled | Cortex-M4 有 CLZ 指令 |

### 6.3 ⚠️ 坑一：必须删掉 `defaultTask`

CubeMX 一启用 FreeRTOS，就自动建一个 `defaultTask`，优先级 **`osPriorityNormal` = 24**。

而我们的控制任务用的是**原生 FreeRTOS 优先级 5**（数值越小越低）。结果：

> **控制任务永远抢不到 CPU，车一动不动，而且没有任何报错。**

**处理**：`Tasks and Queues` 标签页 → 选中 `defaultTask` → **Remove**。

如果 CubeMX 不让删（提示至少要一个任务），就退而求其次：
把它的优先级改成 **`osPriorityLow`**，并在 `StartDefaultTask()` 第一行加 `vTaskSuspend(NULL);`
（自己挂起自己，之后就再也不参与调度了）。

### 6.4 ⚠️ 坑二：`MAX_SYSCALL_INTERRUPT_PRIORITY` 必须是 5

S3 里把 TIM6 设成 6、S4 里把 DMA 设成 6/7，**这些数值只有在 `configMAX_SYSCALL_INTERRUPT_PRIORITY = 5` 的前提下才成立**。

- 如果它是 0（或没配）：优先级 6 的 ISR 调 `FromISR` → 立刻 `configASSERT` 卡死；
- 如果它是 15：S3/S4 设的 6/7 全部失去意义，中断延迟不可控。

CubeMX 的 CMSIS_V2 默认就是 5，生成后**去 `Core/Inc/FreeRTOSConfig.h` 确认一次**：

```c
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5
```

### 6.5 ⚠️ 坑三：堆不够时任务是"静默失败"

`xTaskCreate()` 申请不到内存只会返回 `pdFAIL`，**不报错**。
如果 `TOTAL_HEAP_SIZE` 还是默认的 15360，三个任务 + idle 刚好卡在边缘，多开一个队列就失败。

所以：`TOTAL_HEAP_SIZE = 24576`，并且打开 `USE_MALLOC_FAILED_HOOK`。

### 6.6 生成后必须确认的三件事

1. **时基还是 TIM7**
   `.ioc` 里搜 `TimeBase=TIM7`；`Core/Src/stm32f4xx_hal_timebase_tim.c` 还在。
   > CubeMX 开 FreeRTOS 时**有可能悄悄把 Timebase 改回 SysTick**，那样 HAL 和 FreeRTOS 抢同一个中断，`HAL_Delay()` 会随机卡死。

2. **`main.c` 里有 `osKernelStart()`，且 `MX_FREERTOS_Init()` 里没有 defaultTask**
   ```c
   osKernelInitialize();
   MX_FREERTOS_Init();     /* 里面应只有 USER CODE 段 */
   osKernelStart();
   ```

3. **`Core/Src/app_freertos.c` 或 `Core/Inc/FreeRTOSConfig.h` 的六个数**
   ```c
   configTICK_RATE_HZ                    1000
   configMAX_PRIORITIES                  8
   configTOTAL_HEAP_SIZE                 24576
   configCHECK_FOR_STACK_OVERFLOW        2
   configUSE_TIME_SLICING                0
   configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5
   ```

### 判据

- [ ] `Tasks and Queues` 里只剩空表（或 defaultTask 优先级 = osPriorityLow）
- [ ] `FreeRTOSConfig.h` 六个数全对
- [ ] `.ioc` 里 `NVIC.TimeBaseIP=TIM7` 没变
- [ ] Keil 编译 0 Error，**Code 尺寸比之前大 6–8 KB**（说明 RTOS 真的链进去了）

### 本步不要做

- ❌ 不要在 CubeMX 里建任何任务（`Tasks and Queues` 全留空，任务由 `app_start()` 建）
- ❌ 不要开 `USE_TIMERS`（多一个任务、多 2 KB 栈）
- ❌ 不要开 `USE_IDLE_HOOK`（除非要喂看门狗）
- ❌ 不要接受 CubeMX 把 Timebase 改回 SysTick 的"建议"

## S7 把 fw/ 接进工程

### 7.1 目录安排：用目录联接，保持"单一副本"

Keil 在**中文路径**下容易出怪问题，但文档又放在共享记忆里。解决办法是目录联接（junction）：

```
D:\car\smartcar\fw   ──(junction)──►   D:\默认存储\共享记忆\校赛智能车\fw
```

Keil 看到的是纯 ASCII 路径 `../fw/...`，实际读写的是同一份文件，**不存在两份副本要同步**。

> 注意：如果把整个工程拷到别的电脑，junction 会失效，需要重建或直接复制实体目录。

### 7.2 已完成的源码改动（2026-11-16）

| 文件 | 改动 | 为什么 |
|---|---|---|
| `Core/Src/main.c` | USER CODE Includes 加 `#include "app_main.h"` | |
| `Core/Src/main.c` | USER CODE 2 加 `app_init();` | 在调度器启动前接管外设 |
| `Core/Src/main.c` | USER CODE **Callback 0** 加 `if (htim->Instance == TIM6) { app_tim6_isr(); }` | **不覆盖** CubeMX 的 TIM7 回调 |
| `Core/Src/freertos.c` | USER CODE Includes 加 `#include "app_main.h"` | |
| `Core/Src/freertos.c` | USER CODE RTOS_THREADS 加 `app_start();` | 建任务，必须在 osKernelStart 之前 |
| `Core/Src/freertos.c` | `StartDefaultTask` 里加 `vTaskSuspend(NULL);` | 否则优先级 24 压死控制任务 |
| `fw/app/app_main.c` | **删除**了 `HAL_TIM_PeriodElapsedCallback` 定义 | 避免与 main.c 里的重复定义 |
| `fw/app/app_main.c` | `printf` 重定向改成先判 `__ARMCC_VERSION` | AC6 也定义 `__GNUC__`，判错会让 printf 静默失效 |

**为什么 TIM6 的钩子放在 Callback 0 而不是自己定义回调函数：**
`HAL_TIM_PeriodElapsedCallback` 在 `main.c` 里由 CubeMX 生成（负责 TIM7 → `HAL_IncTick`），
而它在 USER CODE **之外**，重新生成会被重写。放在 Callback 0 段里，**既能挂上 TIM6，又不会被覆盖**。

### 7.3 Keil 工程文件（已用脚本写入）

`.uvprojx` 已自动加入：

- **4 个 Group**：`fw/app`(1) / `fw/ctrl`(2) / `fw/bsp`(7) / `fw/lib`(2)，共 **12 个 .c**
- **4 条 Include Path**：`../fw/app;../fw/ctrl;../fw/bsp;../fw/lib`
- 原文件已备份为 `SmartCar_F407.uvprojx.bak`

### 7.4 ⚠️ 你必须做的一步：重开工程

**Keil 当时正开着这个工程**，它内存里还是旧版，脚本改的 `.uvprojx` 不会自动生效。

```
Keil → Project → Close Project
     → 重新打开 SmartCar_F407.uvprojx
     → 左侧应该出现 fw/app、fw/ctrl、fw/bsp、fw/lib 四个组
     → F7
```

### 7.5 首次编译的排错表

| 报错 | 原因 | 解法 |
|---|---|---|
| `L6218E: Undefined symbol app_init` | `app_main.c` 没进到编译单元 | 检查 `fw/app` 组里有没有 `app_main.c` |
| `cannot open source input file "app_config.h"` | include path 少了 | C/C++ → Include Paths 补 `../fw/app` |
| `L6200E: Symbol HAL_TIM_PeriodElapsedCallback multiply defined` | 旧版 `app_main.c` 还在 | 确认 `app_main.c` 里没有这个函数 |
| `L6915E: Library reports error: __use_no_semihosting` | 用了标准库没实现桩 | 勾 Use MicroLIB，或补 `_sys_exit` 等 4 个空桩 |
| `Undefined symbol vTaskSuspend` | 没 include `task.h` | `freertos.c` 头部本来就有，检查是否被删 |

### 7.6 首次集成结果（2026-11-16）

改用**纯文本插入**改 `.uvprojx`（第一次用 XML 解析器重写整个文件，导致 Keil 打不开，已回滚），最终结果：

```
Program Size: Code=36200  RO-data=1112  RW-data=308  ZI-data=32620
"SmartCar_F407\SmartCar_F407.axf" - 0 Error(s), 0 Warning(s).
```

`Code` 从 18408 → **36200**，说明 12 个 .c 全部参与编译链接。

**过程中踩到并修掉的 3 个问题：**

| # | 现象 | 真实原因 | 修法 |
|---|---|---|---|
| 1 | Keil **打不开工程** | 用 `[xml].Save()` 重写整个 uvprojx，文件被重新序列化（BOM、声明格式、行数全变） | 恢复 `.bak`，改用**纯文本正则插入**，原格式一字节不动 |
| 2 | `cannot open source "../fw/app/app_main.c"` | `fw` 放在了工程**上一层**（`D:\car\smartcar\fw`），而 `../` 只到 `SmartCar_F407\` | 把 junction 移到 `D:\car\smartcar\SmartCar_F407\fw` |
| 3 | `#20: identifier "CFG_W_MAX" is undefined` | `app_config.h` 漏定义（代码里用了，参数表里也写了，就是没加） | 补 `#define CFG_W_MAX 220.0f` |
| 4 | `#9-D: nested comment is not allowed` | 注释里写了 `/* USER CODE BEGIN Callback 0 */`，AC5 认为注释嵌套 | 改成用引号描述，不写 `/*` |

> **教训 1**：不要用 XML 库去改 IDE 的工程文件。文本插入最安全——只加行，不改原有字节。
> **教训 2**：junction 放错层级会导致"文件明明存在但编译器找不到"，因为 Keil 的相对路径基准是 `.uvprojx` 所在的 `MDK-ARM` 目录。

### 判据

- [x] 左侧 4 个组、12 个 .c
- [x] 编译 0 Error / 0 Warning
- [x] `Program Size: Code` = **36200**（> 25000）
- [ ] 串口 115200 能打出 `[car] boot ok, sysclk=168000000` ← **S8 要做的事**

---

## S8 上电自检

目标只有一个：**证明整条链路通了**。不是让车跑，是让串口吐出那一行。

### 8.1 接线（只接三根线）

| 板子 | USB-TTL |
|---|---|
| PA9（DBG_TX，USART1_TX） | **RXD** |
| PA10（DBG_RX，USART1_RX） | **TXD** |
| GND | GND |

> **TX/RX 必须交叉**：板子发、电脑收。
> **只从扩展板 5V 供电，不要同时插核心板 Type-C**（会倒灌）。

**电机、传感器、蓝牙、K230 全部先不接。** 这一步只验证 MCU 本身。

### 8.2 烧录 + 观察

串口助手：**115200 8N1**，无流控。

预期看到：

```
[car] boot ok, sysclk=168000000
```

### 8.3 判据

- [x] 串口打出 `[car] boot ok, sysclk=168000000`（**2026-11-16 21:32 实测通过**）
- [x] 编译 0 Error / 0 Warning，烧录成功
- [ ] **电机完全不动、不抖**（STBY 保持低）—— 待确认
- [ ] 用调试器暂停，能看到 `ctrl_task` / `comm_task` / `ui_task` 三个任务都活着

> **这一行同时证明了五件事**：HSE 起振→PLL→168 MHz 全对；USART1+DMA+`fputc` 重定向通了；
> `HAL_Init`→时钟→所有 `MX_*_Init`→`app_init()` 没卡死；`osKernelStart()` 没崩；任务调度起来了。
>
> **下一步去做台架验证：** 见 [`07_bench.md`](07_bench.md)。固件里内置了文本命令行，
> 不用上位机就能逐个验证 电机 / 编码器 / 循迹 / IMU 的方向和数值。

### 8.4 没输出的排查顺序

按这个顺序查，别跳：

1. **TX/RX 是否交叉**（最常见，占一半）
2. **波特率**：115200，且 `SystemCoreClock` 真是 168 MHz
3. **是否卡在 `app_init()` 里**：Keil 调试模式，在 `app_init()` 第一行下断点，F5 单步走
   - 卡在 IMU 等待循环 → 正常，最多 500 ms 后自己出来
   - 卡在 `HAL_ADC_PollForConversion` → ADC 时钟配错
4. **HardFault**：在 `HardFault_Handler` 下断点。常见原因是任务栈太小或堆不够
5. **时钟**：如果打出 84000000，说明 HSE 没起振（晶振没焊/虚焊），退回了 HSI

### 8.5 关于 printf 的一个限制

工程用的是 **MicroLIB**（`.lnp` 里有 `--library_type=microlib`）。

> **MicroLIB 的 `printf` 不支持 `%f`**（浮点会打成空白或乱码）。
> `app_log` 里我只用了 `%lu`，所以启动信息没问题。
> 以后要看浮点波形，用 `vofa` 那条通道（`%d` 打放大 100 倍后的整数），不要用 `%f`。

### 8.6 编译环境记录

| 项 | 值 |
|---|---|
| 编译器 | **ARMCC V5.06 update 5 (build 528)** —— 即 **AC5**，不是 AC6 |
| C 标准 | C99（`<uC99>1</uC99>`） |
| 运行库 | **MicroLIB** |
| Code / RO / RW / ZI | 36200 / 1112 / 308 / 32620 |

> AC5 与 AC6 的差异要注意：`__weak` 是关键字（不是 `__attribute__((weak))`），
> `BSP_WEAK` 宏已经按 `__CC_ARM` 分支处理好了。
> `__ARMCC_VERSION` 在 AC5/AC6 下都有定义，所以 `fputc` 那个判断两边都成立。

### 8.7 用 HC06 做调试通道（可选）

**结论：能用，而且是最终形态的一部分（无线调参），但 S8 第一次自检建议先有线。**

原因很实在——自检的目的是"确认 MCU 活着"，这时候要**减少变量**。
蓝牙夹在中间会多出三层不确定性：模块本身、AT 配置、Windows 的虚拟 COM 口。
一旦没输出，你会分不清是"固件挂了"还是"蓝牙没连上"，这是最典型的误判。

**推荐顺序**：有线跑通 → 再插蓝牙 → 这时若不出字，立刻能判定是蓝牙侧问题。

#### 接线

| HC06 | 板子 | 说明 |
|---|---|---|
| VCC | 5V | ⚠️ **必须 5V**（模块板载 LDO），接 3.3V 会搜不到 |
| GND | GND | |
| TXD | **PB11**（BT_RX） | 交叉 |
| RXD | **PB10**（BT_TX） | 交叉 |
| STATE | PE15（BT_STATE） | 连上时输出高 |
| KEY | 悬空 | HC06 进 AT 通常不需要拉高 |

HC06 的 TX/RX 都是 3.3V 电平，与 PB10/PB11 直连安全。

#### 把波特率从 9600 改到 115200

出厂 9600，而我们的 USART3 是 115200，**不改就是一串乱码**。

1. 只给 HC06 上电（不配对任何主机），串口助手 **9600 8N1** 连它；
2. 发 `AT` → 回 `OK`（没回说明已被别的设备连着，先断开）；
3. 发 `AT+BAUD8` → 回 `OK+Set:8`
   > 编码表：1=1200 2=2400 3=4800 4=9600 5=19200 6=38400 7=57600 **8=115200** 9=230400
4. 可选 `AT+NAMEcar`、`AT+PSWD1234`；
5. 立即生效，串口助手跟着改 115200。

#### Windows 配对

1. 设置 → 蓝牙和其他设备 → 添加设备 → **蓝牙** → 选 `HC-06`，PIN **1234**；
2. 控制面板 → 设备和打印机 → 右键 HC-06 → 属性 → **服务** → 勾"串行端口"；
3. "更多蓝牙选项 → 端口(COM 和 LPT)"里会出现**两个** COM 口：
   - 标"**传入**"的 —— 打不开，别试
   - 标"**传出**"的 —— 用这个

#### 注意

- HC06 是**经典蓝牙 SPP**，只能做从机、一次连一个主机；**iOS 不支持**，安卓用 SPP 串口 App。
- `app_log()` 现在**同时发 USART1 和 USART3**，只接蓝牙也能看到 `[car] boot ok`。
- 遥测帧（0xAA55+CRC8）和 ASCII 日志混在同一条蓝牙上，串口助手看到的会是"乱码夹文字"——
  **调波形请用 VOFA+/serialplot**，别用串口助手。

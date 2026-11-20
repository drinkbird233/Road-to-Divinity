# STM32F407 智能小车引脚规划（car-V2）

版本：2026-10-27  
主控：STM32F407ZGT6 核心板 **LXBF407ZG-P1 V2.0（鹿小班）**  
用途：给代码生成对话直接使用；网络名、模式、时钟、协议均按本文件为准。

---

## 1. 硬件清单

| 部件 | 型号/说明 |
|---|---|
| 主控 | STM32F407ZGT6，核心板 LXBF407ZG-P1 V2.0 |
| 视觉 | CanMV-K230，UART 通信 |
| 陀螺仪 | HWT101，UART 通信 |
| 电机驱动 | TB6612 模块，驱动两路 JGB 编码电机 |
| 电机 | JGB 12V，AB 两相编码器，5V 编码器供电，堵转 ≤1.2A |
| 循迹 | 八路 I2C 循迹模块，地址 0x12，5V 供电 |
| 蓝牙 | HC06，经典蓝牙 SPP，用于上位机收发 |
| 显示 | 0.96 寸 OLED，SSD1306，I2C 地址 0x3C |
| 电池 | 3S LiPo，9.0–12.6V |
| 电源 | 5V/5A 降压模块；核心板板载 RY3420 产生 3.3V |
| 接插件 | PH2.0（传感器/电机），电池用 XT30 |

---

## 2. 核心板资源与限制

核心板尺寸 74.5 × 43.5 mm，上下各一组 **2×29P、2.54 mm 排针**，外排距 40.0 mm。  
扩展板直接使用 `封装库（F103&407通用）.epro` 中的 H2/H3 封装，网络名按核心板背面丝印对应。

### 2.1 板载已占用引脚（禁止复用）

| 功能 | 占用引脚 |
|---|---|
| MicroSD | PC8、PC9、PC10、PC11、PC12、PD2 |
| Type-C USB | PA11、PA12 |
| 10-pin FPC LCD | PB3、PB5、PD11、PD12、PD13 |
| 板载按键 | PA15、PE6 |
| 板载 LED | PC13 |
| 调试口 H1 | PA9、PA10（USART1）；PA13、PA14（SWD） |
| 8 MHz 晶振 | PH0、PH1 |
| 32.768 kHz 晶振 | PC14、PC15 |
| BOOT1 | PB2 |

> PA9/PA10 允许用于调试串口 USART1；PA13/PA14 只用于 SWD，不做普通 GPIO。

---

## 3. 引脚总表

### 3.1 电机与编码器

| 网络名 | MCU 引脚 | 模式 | 说明 |
|---|---|---|---|
| MOTOR_L_PWM | PE9 | TIM1_CH1 AF1 | 20 kHz PWM |
| MOTOR_L_IN1 | PE8 | GPIO 输出 | 10k 下拉 |
| MOTOR_L_IN2 | PE10 | GPIO 输出 | 10k 下拉 |
| MOTOR_R_PWM | PE11 | TIM1_CH2 AF1 | 20 kHz PWM |
| MOTOR_R_IN1 | PE12 | GPIO 输出 | 10k 下拉 |
| MOTOR_R_IN2 | PE13 | GPIO 输出 | 10k 下拉 |
| MOTOR_STBY | PE14 | GPIO 输出 | 10k 下拉，默认低 |
| ENC_L_A | PB6 | TIM4_CH1 AF2 | 5V 输入，100Ω + 10nF |
| ENC_L_B | PB7 | TIM4_CH2 AF2 | 5V 输入，100Ω + 10nF |
| ENC_R_A | PA6 | TIM3_CH1 AF2 | 5V 输入，100Ω + 10nF |
| ENC_R_B | PA7 | TIM3_CH2 AF2 | 5V 输入，100Ω + 10nF |

### 3.2 通信

| 网络名 | MCU 引脚 | 模式 | 说明 |
|---|---|---|---|
| IMU_RX | PA3 | USART2_RX AF7 | HWT101 TX → MCU |
| IMU_TX | PA2 | USART2_TX AF7 | MCU → HWT101 RX |
| BT_TX | PB10 | USART3_TX AF7 | MCU → HC06 RX |
| BT_RX | PB11 | USART3_RX AF7 | HC06 TX → MCU |
| BT_STATE | PE15 | GPIO 输入 | HC06 连接状态，10k 下拉 |
| K230_TX | PC6 | USART6_TX AF8 | MCU → K230 RX |
| K230_RX | PC7 | USART6_RX AF8 | K230 TX → MCU |
| DBG_TX | PA9 | USART1_TX AF7 | 调试串口，核心板 H1 |
| DBG_RX | PA10 | USART1_RX AF7 | 调试串口，核心板 H1 |
| OLED_SCL | PB8 | I2C1_SCL AF4 | 3.3V，4.7k 上拉 |
| OLED_SDA | PB9 | I2C1_SDA AF4 | 3.3V，4.7k 上拉 |
| TRACK_SCL | PD3 | GPIO 开漏 | 软件 I2C，5V 总线，4.7k 上拉 |
| TRACK_SDA | PD4 | GPIO 开漏 | 软件 I2C，5V 总线，4.7k 上拉 |

### 3.3 人机与检测

| 网络名 | MCU 引脚 | 模式 | 说明 |
|---|---|---|---|
| BUZZER | PC0 | GPIO 输出 | 高电平响，NPN 驱动 |
| KEY1 | PC1 | GPIO 输入 | 10k 上拉，按下为 0 |
| KEY2 | PC2 | GPIO 输入 | 10k 上拉，按下为 0 |
| KEY3 | PC3 | GPIO 输入 | 10k 上拉，按下为 0 |
| LED1 | PC4 | GPIO 输出 | 高电平亮，1k 限流 |
| LED2 | PC5 | GPIO 输出 | 高电平亮，1k 限流 |
| VBAT_ADC | PA4 | ADC1_IN4 | 47k/10k 分压，100nF |

### 3.4 预留

| 功能 | 引脚 | 说明 |
|---|---|---|
| SPI2 | PB13 SCK / PB14 MISO / PB15 MOSI / PB12 CS | 3.3V，2.54 排针 |
| CAN1 | PD0 RX / PD1 TX | 可选 |
| 备用 GPIO | PF0–PF15、PG0–PG15、PE0–PE5、PE7 | 2×10 排针，带 3.3V/GND |

---

## 4. CubeMX 配置

### 4.1 时钟

- HSE：8 MHz 晶振
- SYSCLK：168 MHz
- AHB：168 MHz
- APB1：42 MHz（TIM3/TIM4 时钟 84 MHz）
- APB2：84 MHz（TIM1 时钟 168 MHz）

### 4.2 定时器

| 定时器 | 配置 | 用途 |
|---|---|---|
| TIM1 | CH1/CH2 PWM，PSC=0，ARR=8399，边沿对齐 | 20 kHz 电机 PWM |
| TIM3 | Encoder Mode，TI1/TI2 both edges，PSC=0，ARR=65535 | 右编码器 |
| TIM4 | Encoder Mode，TI1/TI2 both edges，PSC=0，ARR=65535 | 左编码器 |
| TIM6 | PSC=8399，ARR=99，更新中断 | 10 ms 控制节拍 |

TIM3/TIM4 为 16 位计数器。每 10 ms 读取一次：

```c
uint16_t now = __HAL_TIM_GET_COUNTER(&htimx);
int16_t delta = (int16_t)(now - last);
last = now;
```

### 4.3 串口

| 串口 | 引脚 | 波特率 | 用途 | DMA |
|---|---|---|---|---|
| USART1 | PA9/PA10 | 115200 8N1 | 调试 | 可选 |
| USART2 | PA2/PA3 | 9600 8N1（按 HWT101 手册） | HWT101 | 可选 |
| USART3 | PB10/PB11 | 115200 8N1 | HC06 上位机 | RX：DMA1 Stream1 Ch4 |
| USART6 | PC6/PC7 | 115200 8N1（与 K230 约定） | K230 | RX：DMA2 Stream1 Ch5 |

- USART3/USART6 建议使用 **DMA + IDLE 中断** 收包，禁止阻塞式接收。
- CubeMX 自动分配 DMA 时以上表为参考；若手工配置，按 F407 DMA 映射表核对。
- 所有外部串口 TX/RX 串联 100Ω。

### 4.4 I2C 与 ADC

| 外设 | 配置 | 用途 |
|---|---|---|
| I2C1 | 400 kHz，7-bit，PB8/PB9，开漏 | OLED |
| 软件 I2C | 100 kHz，PD3/PD4，开漏 | 八路循迹 |
| ADC1 | 12-bit，PA4，采样时间 144 cycles | 电池电压 |

### 4.5 GPIO 初始状态

- MOTOR_STBY = 0（驱动关闭）
- MOTOR_L_IN1/IN2 = 0，MOTOR_R_IN1/IN2 = 0
- PWM 占空比 = 0
- BUZZER = 0
- LED1/LED2 = 0
- 软件 I2C 的 PD3/PD4 初始为高电平（开漏释放）
- BT_STATE 输入，10k 下拉
- KEY1–KEY3 输入，外部 10k 上拉（也可开内部上拉）

### 4.6 NVIC 建议

| 中断 | 优先级 |
|---|---|
| TIM6 10 ms 控制节拍 | 最高 |
| USART3/USART6 DMA + IDLE | 中 |
| I2C1 / ADC | 低 |

---

## 5. 外设协议

### 5.1 OLED

- 型号：SSD1306，0.96 寸
- 7-bit 地址：`0x3C`（部分模块 0x3D）
- 总线：I2C1，400 kHz
- 上拉：4.7k 到 3.3V

### 5.2 八路循迹模块

- 供电：5V
- I2C 地址：`0x12`
- 数据寄存器：`0x30`，读 1 字节
- 位映射：bit0 = x1，bit1 = x2，……，bit7 = x8
- 黑线电平需实测，代码用宏：

```c
#define TRACK_BLACK_LEVEL 0   // TODO：实测后改为 0 或 1
```

- 软件 I2C，100 kHz；PD3=SCL，PD4=SDA，开漏，外部 4.7k 上拉到 5V。
- 循迹误差建议用加权平均：

```text
weights = {-3.5, -2.5, -1.5, -0.5, +0.5, +1.5, +2.5, +3.5}
error = sum(weight[i] * active[i]) / sum(active[i])
```

### 5.3 HWT101

- UART：PA2/PA3，默认 9600 8N1（以模块手册为准，可改 115200）
- 维特智能协议：帧头 `0x55`，11 字节一帧，第 11 字节为前 10 字节和校验
- 解析出 yaw、gyro_z；用于航向保持/转向闭环
- 上电后先等待模块稳定，再开始解析

### 5.4 K230

- UART：PC6/PC7，默认 115200 8N1（与 K230 端约定）
- 建议帧格式与蓝牙一致：`0xAA 0x55 | TYPE | LEN | PAYLOAD | CRC8`
- K230 → STM32，TYPE = `0x10`，PAYLOAD 8 字节：

| 字段 | 类型 | 说明 |
|---|---|---|
| valid | uint8 | 1=检测到目标，0=无目标 |
| target_x | int16 | 目标横坐标，建议归一化到 -1000~1000 |
| target_y | int16 | 目标纵坐标，建议归一化到 -1000~1000 |
| frame_id | uint16 | 帧序号 |
| label | uint8 | 目标类别 |

- 超过 100 ms 未收到新帧，视为视觉失效。

### 5.5 HC06 上位机通信

- UART：PB10/PB11，115200 8N1
- 经典蓝牙 SPP；PC/Android 可用，iOS 不支持
- 帧格式：

```text
0xAA 0x55 | TYPE(1) | LEN(1) | PAYLOAD(LEN) | CRC8(1)
```

- CRC8：多项式 `0x07`，初值 `0x00`，计算范围 `TYPE + LEN + PAYLOAD`
- 小端序

#### 遥测帧：TYPE = 0x01，LEN = 17

| 字段 | 类型 | 单位/说明 |
|---|---|---|
| tick_ms | uint16 | 系统运行毫秒 |
| speed_l | int16 | 左轮速度，mm/s |
| speed_r | int16 | 右轮速度，mm/s |
| yaw | int16 | 航向角，0.1° |
| gyro_z | int16 | 角速度，0.1°/s |
| track_mask | uint8 | 八路循迹状态 |
| vbat_mv | uint16 | 电池电压，mV |
| target_x | int8 | K230 目标横坐标（缩放到 -100~100） |
| target_y | int8 | K230 目标纵坐标（缩放到 -100~100） |
| mode | uint8 | 0=空闲，1=循迹，2=视觉，3=遥控 |
| status | uint8 | bit0=IMU正常，bit1=视觉有效，bit2=蓝牙连接，bit3=低电压 |

#### 命令帧：TYPE = 0x02

| cmd | 名称 | 参数 |
|---|---|---|
| 0x01 | START | 无 |
| 0x02 | STOP | 无 |
| 0x03 | SET_SPEED | int16 left_mm_s, int16 right_mm_s |
| 0x04 | SET_PID | uint8 loop, int16 kp, int16 ki, int16 kd（缩放系数自定义） |
| 0x05 | SET_MODE | uint8 mode |
| 0x06 | REQUEST_TELEMETRY | 无 |
| 0x07 | CALIBRATE_IMU | 无 |
| 0x08 | SET_STREAM_RATE | uint8 Hz（建议 10–50） |

#### 应答帧：TYPE = 0x82

| 字段 | 类型 | 说明 |
|---|---|---|
| cmd | uint8 | 对应命令 |
| result | uint8 | 0=成功，其他=错误码 |

- 遥测建议 20–50 Hz。
- **失效保护**：500 ms 未收到上位机命令，PWM=0、STBY=0。

---

## 6. 控制逻辑建议

### 6.1 10 ms 控制节拍

1. 读取 TIM3/TIM4 编码器计数，计算左右轮速度。
2. 读取 HWT101 最新 yaw/gyro。
3. 读取循迹模块状态、K230 最新目标。
4. 根据模式计算目标速度：
   - 模式 0：目标速度 0。
   - 模式 1：循迹误差 → 差速。
   - 模式 2：K230 target_x → 转向；target_y/距离 → 速度。
   - 模式 3：上位机直接给左右目标速度。
5. 速度 PID → PWM。
6. 更新电机方向与 PWM。
7. 调度遥测发送（20–50 Hz）。
8. 检查失效保护、低电压、急停。

### 6.2 电机方向真值表

| IN1 | IN2 | 状态 |
|---|---|---|
| 1 | 0 | 正转 |
| 0 | 1 | 反转 |
| 0 | 0 | 滑行停止 |
| 1 | 1 | 刹车 |

- PWM：0–8399。
- 上电顺序：STBY=0，PWM=0，方向=0 → 延时 100 ms → STBY=1。
- 故障/失联/低电压：PWM=0，STBY=0。
- 换向前先降 PWM，避免硬切正反转。

### 6.3 速度换算

```c
#define ENCODER_PPR        11      // TODO：按电机铭牌修改
#define GEAR_RATIO         30.0f   // TODO：按减速比修改
#define ENCODER_CPR        (ENCODER_PPR * 4.0f * GEAR_RATIO)
#define WHEEL_DIAMETER_MM  65.0f   // TODO：按实车轮径修改
#define WHEEL_CIRC_MM      (3.1415926f * WHEEL_DIAMETER_MM)
#define MM_PER_COUNT       (WHEEL_CIRC_MM / ENCODER_CPR)

// delta 为 10 ms 内的计数差
float speed_mm_s = delta * MM_PER_COUNT * 100.0f;
```

### 6.4 电池检测

```c
#define VBAT_R_TOP     47000.0f
#define VBAT_R_BOTTOM  10000.0f
#define VBAT_SCALE     ((VBAT_R_TOP + VBAT_R_BOTTOM) / VBAT_R_BOTTOM)  // 5.7

float vbat = adc_raw * 3.3f / 4095.0f * VBAT_SCALE;
```

3S LiPo 建议阈值：

```c
#define VBAT_WARN_MV  10500   // 3.50V/cell，报警
#define VBAT_STOP_MV   9900   // 3.30V/cell，停车
```

### 6.5 状态机

```text
INIT → IDLE → TRACK / VISION / REMOTE → FAULT
```

- `INIT`：外设初始化、IMU 稳定、电机保持关闭。
- `IDLE`：电机停，允许切换模式。
- `TRACK`：循迹闭环。
- `VISION`：K230 目标跟踪。
- `REMOTE`：上位机直接控制。
- `FAULT`：低电压、失联、IMU 异常、电机异常，PWM=0、STBY=0。
- 任意状态收到 STOP/急停 → IDLE/FAULT。

---

## 7. 连接器定义

| 连接器 | 类型 | 引脚定义 |
|---|---|---|
| J1 电池 | XT30 | 1=+，2=− |
| J2 左电机 | PH2.0 6-pin | 1=M+，2=M−，3=5V，4=GND，5=A，6=B |
| J3 右电机 | PH2.0 6-pin | 1=M+，2=M−，3=5V，4=GND，5=A，6=B |
| J4 循迹 | PH2.0 4-pin | 1=5V，2=GND，3=SCL，4=SDA |
| J5 OLED | PH2.0 4-pin | 1=GND，2=3.3V，3=SCL，4=SDA |
| J6 HC06 | PH2.0 6-pin | 1=5V，2=GND，3=TXD，4=RXD，5=STATE，6=KEY |
| J7 HWT101 | PH2.0 4-pin | 1=3.3V，2=GND，3=TX，4=RX |
| J8 K230 | PH2.0 6-pin | 1=5V，2=5V，3=GND，4=GND，5=TX，6=RX |
| J9 调试 | 2.54 4-pin | 1=3.3V，2=GND，3=TX，4=RX |
| J10 SWD | 2.54 5-pin | 1=3.3V，2=GND，3=SWDIO，4=SWCLK，5=NRST |
| J11 SPI2 | 2.54 2×4 | 3.3V、GND、SCK、MISO、MOSI、CS |
| J12 电源输出 | PH2.0 2-pin | 5V/GND、3.3V/GND 各一组 |

> 所有模块的 TX/RX 都是交叉连接：模块 TX → MCU RX，模块 RX ← MCU TX。

---

## 8. 电源与安全

### 8.1 电源树

```text
3S LiPo 9.0–12.6V
  └─ XT30 → 5A 保险丝 → 电源开关 → VM
       ├─ TB6612 VM
       └─ 5V/5A 降压模块 → 5V
            ├─ 核心板 5V
            │    └─ 核心板 RY3420 → 3.3V
            │         ├─ STM32
            │         ├─ OLED
            │         ├─ TB6612 VCC
            │         └─ 上拉/指示灯
            ├─ K230 5V
            ├─ HC06 5V
            ├─ HWT101（3.3V 或 5V，按模块）
            ├─ 循迹模块 5V
            ├─ 编码器 5V
            └─ 蜂鸣器 5V
```

- 核心板 Type-C 与扩展板 5V **不要同时供电**；加跳线或二极管。
- 不要额外并 3.3V 模块到核心板 3.3V；如需独立 3.3V，必须用跳线隔离，避免倒灌。
- TB6612 VM 旁加 100µF/25V + 100nF。
- K230 连接器旁加 470µF。
- 每个传感器接口就近加 100nF。
- 电机地、电源地、信号地单点汇合；编码器线与电机线分开走。

### 8.2 TB6612

- 单路连续 1.2A，峰值 3.2A；电机堵转 ≤1.2A，余量很小。
- 模块加散热片，5A 保险丝，软件加减速斜坡。
- 所有输入加 10k 下拉，STBY 默认低。
- VM 上限 13.5V，3S 满电 12.6V，不要用 4S。

### 8.3 失效保护

| 条件 | 动作 |
|---|---|
| 500 ms 无上位机命令 | PWM=0，STBY=0 |
| 电池 < 9.9V | 停车，LED/蜂鸣器报警 |
| IMU 连续 200 ms 无数据 | 视觉/循迹模式降级或停车 |
| K230 连续 100 ms 无数据 | 视觉模式停车 |
| 电机驱动故障/过流 | STBY=0，进入 FAULT |

---

## 9. 代码常量汇总

```c
// 时钟
#define SYS_CLK_HZ              168000000U
#define APB1_TIM_CLK_HZ         84000000U
#define APB2_TIM_CLK_HZ         168000000U

// 电机
#define MOTOR_PWM_MAX           8399U
#define MOTOR_PWM_FREQ_HZ       20000U
#define MOTOR_L_INVERT          0       // TODO：按实际安装方向修改
#define MOTOR_R_INVERT          0       // TODO：按实际安装方向修改

// 编码器
#define ENCODER_PPR             11      // TODO：按电机铭牌修改
#define GEAR_RATIO              30.0f   // TODO：按减速比修改
#define ENCODER_CPR             (ENCODER_PPR * 4.0f * GEAR_RATIO)
#define CONTROL_PERIOD_MS       10U

// 电池
#define VBAT_R_TOP              47000.0f
#define VBAT_R_BOTTOM           10000.0f
#define VBAT_WARN_MV            10500U
#define VBAT_STOP_MV            9900U

// 循迹
#define TRACK_I2C_ADDR          0x12U
#define TRACK_REG_DATA          0x30U
#define TRACK_BLACK_LEVEL       0       // TODO：实测确认 0 或 1

// OLED
#define OLED_I2C_ADDR           0x3CU

// 通信
#define BT_BAUD                 115200U
#define K230_BAUD               115200U
#define IMU_BAUD                9600U   // TODO：按 HWT101 手册确认
#define TELEMETRY_PERIOD_MS     20U
#define FAILSAFE_TIMEOUT_MS     500U
#define VISION_TIMEOUT_MS       100U
#define IMU_TIMEOUT_MS          200U

// 协议
#define FRAME_HEAD_0            0xAAU
#define FRAME_HEAD_1            0x55U
#define FRAME_TYPE_TELEMETRY    0x01U
#define FRAME_TYPE_COMMAND      0x02U
#define FRAME_TYPE_ACK          0x82U
#define FRAME_TYPE_VISION       0x10U
```

---

## 10. 待实测/待填写参数

| 参数 | 说明 |
|---|---|
| `ENCODER_PPR` | JGB 电机编码器线数，常见 11 或 13 |
| `GEAR_RATIO` | 电机减速比，按铭牌填写 |
| `WHEEL_DIAMETER_MM` | 车轮直径 |
| `MOTOR_L_INVERT` / `MOTOR_R_INVERT` | 电机安装方向 |
| `TRACK_BLACK_LEVEL` | 黑线对应 0 还是 1 |
| `IMU_BAUD` | HWT101 实际波特率 |
| `K230_BAUD` | 与 K230 端约定 |
| TB6612 模块排针顺序 | 按模块丝印，网络名不要按位置猜 |
| HC06 模块引脚顺序 | 按模块丝印，TX/RX 交叉 |
| 车盘尺寸/固定孔 | 扩展板板框 80×50 mm 为默认，按车盘调整 |

---

## 11. 禁止占用引脚清单

| 引脚 | 原因 |
|---|---|
| PC8–PC12、PD2 | MicroSD / SDIO |
| PB3、PB5、PD11、PD12、PD13 | 板载 FPC LCD |
| PA15、PE6 | 板载按键 |
| PC13 | 板载 LED |
| PA11、PA12 | Type-C USB |
| PA13、PA14 | SWD |
| PH0、PH1 | 8 MHz 晶振 |
| PC14、PC15 | 32.768 kHz 晶振 |
| PB2 | BOOT1 |

---

## 12. 代码生成提示

- 使用 STM32CubeMX + HAL 库，生成 Keil/CubeIDE 工程。
- 先完成 GPIO、时钟、TIM1 PWM、TIM3/TIM4 编码器、TIM6 10 ms 中断。
- 再完成 USART3/USART6 的 DMA + IDLE 收包和帧解析。
- I2C1 用硬件 I2C；循迹用 PD3/PD4 软件 I2C。
- 所有中断服务函数只做标志/入队，业务逻辑放到主循环或 10 ms 节拍。
- 电机上电默认关闭，任何异常先 `STBY=0`。
- 先调通电机正反转和编码器计数方向，再闭环。
- 先调通 HC06 收发，再接入循迹/视觉模式。
- 保留 `DEBUG_UART` 输出，方便 printf 调试。

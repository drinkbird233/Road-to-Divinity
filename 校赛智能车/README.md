# 校赛智能车（STM32F407ZGT6）

面向校内智能车竞赛的循迹小车。8 路灰度循迹 + HWT101 陀螺仪 + K230 视觉 + 双编码器速度闭环，
HAL + FreeRTOS，5 ms 控制节拍。

## 功能

- **循迹**：8 路灰度（I2C 0x12），加权误差 + 低通，转向环输出角速度目标
- **姿态**：HWT101（100 Hz 角速度 / 100 Hz 角度），做角速度内环与航向保持
- **速度**：TIM3/TIM4 正交编码器，1320 count/圈，双轮独立 PID + 前馈
- **视觉**：K230 串口帧（`0xAA55` + LEN + PAYLOAD + CRC8），辅助转向
- **元素状态机**：直道 / 弯道 / S 弯 / 盲走 / 圆环岛 / 六边形环岛 / 十字 / 停车
- **台架命令行**：USART1（CH340）与蓝牙 HC06 双通道，可实时改参数、看曲线

## 硬件

| 部件 | 型号 | 接口 / 引脚 |
|---|---|---|
| 主控 | STM32F407ZGT6 | 168 MHz，LQFP144 |
| 电机驱动 | TB6612FNG ×2 | TIM1_CH1/CH2 PWM，PE8/PE9/PE10/PE11/PE13/PE14 |
| 电机 | JGB37-520 12V 30:1 | 11 线 AB 霍尔，1320 count/圈 |
| 编码器 | — | 左 TIM4(PB6/PB7)，右 TIM3(PA6/PA7) |
| 循迹 | 8 路灰度 | I2C 0x12，PD3/PD4 软件 I2C |
| 陀螺仪 | HWT101 | USART2，115200，100 Hz |
| 视觉 | K230 | USART6，115200 |
| 蓝牙 | HC06 | USART3，115200 |
| 调试 | CH340 | USART1，115200 |

## 关键实测参数

| 项 | 值 | 说明 |
|---|---|---|
| 编码器 | 1320 count/圈 | 11 PPR × 4 倍频 × 30 减速比 |
| 轮径 | 65 mm | 0.1547 mm/count |
| 速度环前馈 | 10.4 duty/(mm/s) | 实测 2650 duty → 253 mm/s |
| 控制周期 | 5 ms（实测 188~200 Hz） | TIM6 + `xTaskNotifyGiveFromISR` |
| 循迹 I2C | **必须 25 kHz** | 见下方踩坑 |
| 循迹位序 | L1→`0x7F` … L8→`0xFE` | 0 = 压黑线，1 = 白 |

## 构建方式

1. Keil MDK 5，编译器 **ARMCC 5.06**（AC5），勾选 MicroLIB
2. 打开 `MDK-ARM/SmartCar_F407.uvprojx`，F7 编译、F8 下载
3. CubeMX 工程为 `SmartCar_F407.ioc`（Timebase = TIM7，TIM6 = 5 ms 节拍）

## 台架命令

```
h            帮助
m <l> <r>    直接给占空比（开环）
r <l> <r>    速度闭环（mm/s）
x            停车
g <kp> <ki> <ff>   改速度环增益（×10）
o            50 ms 速度曲线（目标 / 实测 / duty）
w            200 ms 编码器监控
d            循迹 8 位连续显示
t            循迹单次（打印 L1..L8 命中）
i            IMU 姿态
v            整车状态（mode / fault / vbat / 控制节拍数）
p            电机 / 引脚 / 定时器状态
e / k        编码器计数 / 编码器定时器寄存器
z <us>       改循迹 I2C 半周期（排查用）
f            扫描循迹模块 I2C 寄存器
y            遥测帧开关（默认关，会污染日志）
q            IMU 实际帧率
n            编码器引脚跳变探针
```

## 踩坑记录

- **循迹 I2C 速度**：模块的从机是软件模拟的，200 kHz 时丢前 3 位（读到的值像"右移 3 位"），
  50 kHz 丢 1 位，**25 kHz 才完整**。表现为"只有 5 路能用""L1 显示成 L4"，
  曾误判为模块固件 bug。降到 25 kHz 后与手册逐值吻合。
- **软 I2C 超时不能太长**：`scl_hi()` 等从机释放 SCL 的循环若超时过长，
  总线故障时会在最高优先级的控制任务里烧掉 CPU，导致命令行完全无响应。
- **遥测别和日志共用串口**：50 Hz 二进制帧会把文本日志挤成乱码，默认关闭。
- **TB6612 只有 1.2 A**：JGB37-520 堵转 2.5~3 A，所以有斜坡和占空比上限保护。
- **上电必须默认 IDLE**：按键上电即被读成"按下"会让车自动起步，已改为按实际电平初始化。

## 目录

```
fw/         本项目代码（app / ctrl / bsp / lib / docs）
Core/       CubeMX 生成
Drivers/    ST HAL + CMSIS
Middlewares/ FreeRTOS
MDK-ARM/    Keil 工程
docs/       pin_plan.md、schematic_checklist.md
```

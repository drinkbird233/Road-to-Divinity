/**
 * @file  app_config.h
 * @brief 全车唯一参数中心。改参数只改这里（在线整定项�?car_params_t）�? *
 * 单位约定�? *   速度 mm/s、角�?0.1°、角速度 0.1°/s、占空比 0..8399、误�?权重单位"�? = 一个传感器间距�? *
 * �?[TODO] 的必须在实车上量出来，不要抄铭牌�? */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ======================= 调度 ======================= */
#define CFG_CTRL_PERIOD_MS      5u    /* 控制节拍，TIM6 触发 */
#define CFG_FSM_DIVIDER         2u    /* 状态机�?2 拍跑一�?�?10 ms */
#define CFG_COMM_POLL_MS        2u    /* 串口轮询周期 */
#define CFG_TELEM_PERIOD_MS     20u   /* 遥测 50 Hz */
/* 遥测默认**关闭**：它和文本日志共用同一个蓝牙口�?0 Hz 的二进制帧会�? * 命令行输出冲得七零八落（乱码、截断、丢行）�? * 台架阶段保持 0；需要看波形时用命令 y 打开�?*/
#define CFG_TELEM_ENABLE        0
#define CFG_UI_PERIOD_MS        100u  /* OLED / 按键 10 Hz */

/* 按键重映射（物理键序号 0/1/2 = KEY1/KEY2/KEY3，对应 PC1/PC2/PC3）。
 * 2026-11-20 实测：KEY1(PC1) 一直为低电平，读不到按下跳变（硬件问题），
 * 所以把启动键换到 KEY2，IMU 零位先挂在坏掉的 KEY1 上（不会误触发）。 */
#define CFG_KEY_START_IDX       1u    /* start/stop */
#define CFG_KEY_IMU_IDX         0u    /* IMU zero (KEY1 is broken on this board) */
#define CFG_KEY_ESTOP_IDX       2u    /* emergency stop */

/* ======================= 机械 =======================
 * 电机 JGB37-520 / 12V / 减速比 30 / AB 双相磁性霍尔编码器�?1 线）
 *   输出轴空�?�?330 rpm �?65 mm �?�?1.12 m/s 空载，带载预�?0.7�?.9
 *   编码器在电机轴上�?1 × 4 × 30 = 1320 count / 输出轴圈
 *   [TODO] 上车先手转一圈数脉冲，确认到底是 11 线还�?13 �? */
#define CFG_ENCODER_PPR         11.0f
#define CFG_ENCODER_MULT        4.0f  /* 编码器模�?TI1/TI2 both edges = 4 倍频 */
#define CFG_GEAR_RATIO          30.0f
#define CFG_WHEEL_DIAMETER_MM   65.0f
#define CFG_WHEEL_BASE_MM       150.0f
#define CFG_ENCODER_CPR         (CFG_ENCODER_PPR * CFG_ENCODER_MULT * CFG_GEAR_RATIO)
#define CFG_MM_PER_COUNT        (3.14159265f * CFG_WHEEL_DIAMETER_MM / CFG_ENCODER_CPR)

/* 左右编码器通道对调�? * 2026-11-19 实测：转左轮时变的是 R �?左电机的编码器接�?PA6/PA7(TIM3) 上，
 * 所以置 1，让 L �?TIM3�?*/
#define CFG_ENCODER_SWAP_LR     1

/* 编码器计数方向翻转�? * 2026-11-19 实测：两轮向前转�?L 减、R 增�? * 交换�?L �?TIM3（向前增，不用翻），R �?TIM4（向前减，要翻）�?*/
#define CFG_ENCODER_L_INVERT    0
#define CFG_ENCODER_R_INVERT    1

/* ======================= 电机 / TB6612 =======================
 * ⚠️ TB6612 连续只有 1.2 A，JGB37-520�?20 电机�?2V 堵转�?2.5�? A�? *    正常跑没问题，起�?卡住会超。所以斜坡和占空比上限是安全装置，不�?可调�?�? *    备选：DRV8871�?.6 A）或 TB6643�? */
#define CFG_PWM_MAX             8399.0f
/* 电机方向：置 1 = 把占空比取反，用来纠�?M+/M�?接反"�?电机装反"�? * 2026-11-19 实测：两�?M+/M�?都反接（�?m -800 -800 才前进），所以两个都�?1�? * 以后如果重新接线，记得把这里改回来�?*/
#define CFG_MOTOR_L_INVERT      1
#define CFG_MOTOR_R_INVERT      1
/* 电机左右通道对调：如�?TIM1_CH1(PE9) 实际驱动的是右轮，置 1�? * 判据：m 800 0 转的应该�?左轮"�?*/
#define CFG_MOTOR_SWAP_LR       0
#define CFG_DUTY_SLEW           300.0f/* 每个 5 ms 周期最大占空比变化（防冲击电流�?*/
#define CFG_MOTOR_DEADBAND      60.0f /* 低于此占空比轮子不转，做补偿 */
#define CFG_V_MAX               1000.0f

/* ======================= 速度环（内环�?======================= */
#define CFG_SPEED_FF            9.00f  /* feedforward: duty per mm/s. measured steady duty 2685 at 300 mm/s => 8.95 */
#define CFG_SPEED_KP            8.00f  /* P: measured 6.0 was too soft */
#define CFG_SPEED_KI            80.0f  /* I: with correct FF it only trims */
#define CFG_SPEED_I_BAND        10.0f /* integral separation: pause I when |err| > this (mm/s).
                                       * 30 时超调仍有 26%（误差一进 30 就积分又推过头），收窄到 10。
                                       * 稳态误差 <1 mm/s，所以积分在带内仍然有效。 */
#define CFG_SPEED_I_LIM         2500.0f
#define CFG_SPEED_LPF_HZ        30.0f /* 速度测量低�?*/

/* ======================= 转向（外�?+ 角速度环） ======================= */
/* 位置环：误差 �?期望角速度。只�?想转多快"，不含陀螺仪（避免双阻尼导致稳态不转） */
#define CFG_STEER_KP            70.0f /* deg/s per pitch. 45 was too slow for corners */
#define CFG_STEER_KP_MAX        220.0f/* clamp for the steering term */
#define CFG_STEER_KD            6.0f  /* deg/s per (pitch/s): derivative on line error, anticipates corners */
/* 角速度环：Δv = RATE_FF*ω_ref + RATE_KP*(ω_ref �?ω_meas) */
#define CFG_RATE_FF             (CFG_WHEEL_BASE_MM * 0.5f * 0.0174533f) /* mm/s per °/s */
#define CFG_RATE_KP             0.35f /* mm/s per °/s，只做动态修�?*/
#define CFG_DV_MAX              420.0f/* 差速限�?mm/s */
#define CFG_W_MAX               220.0f/* 期望角速度上限 °/s */
/* 直道航向保持：ω_ref += KYAW*(yaw_ref �?yaw) */
#define CFG_YAW_HOLD_KP         0.80f /* °/s per ° */
#define CFG_YAW_HOLD_ERR_MAX    1.20f /* |e| 小于此值才启用航向保持 */

#define CFG_ERR_LPF_HZ          20.0f

/* ======================= 视觉 ======================= */
#define CFG_VIS_TIMEOUT_MS      100u
#define CFG_VIS_TAU_MS          80.0f /* [TODO] 端到端延迟实�?*/
#define CFG_VIS_KFF             0.35f /* 前馈增益，宁小勿�?*/

/* ======================= 循迹 ======================= */
#define CFG_TRACK_ADDR          0x12u
#define CFG_TRACK_REG           0x30u
/* 读取方式�? = 直接读一字节（START→地址读→收→STOP，无重复起始�? *           0 = 传统寄存器读（写寄存器地址后重复起始）
 * 依据：f 命令�?0x00~0x3F 全部返回同一字节 �?模块不分寄存器�? * 传统方式实测会让数据右移 3 位（L1 本该�?0x7F，读�?0xEF）�?*/
#define CFG_TRACK_READ_MODE     1u
/* 补偿开关：2026-11-20 已查�?右移 3 �?�?*我们读得太快**导致的时序问�? * （z 20 �?25 kHz 时模块返回正确的 0x7F）。所以不需要补偿，�?0�? * 速度�?CFG_TRACK_I2C_HALF_US 控制�?*/
#define CFG_TRACK_SHIFT3        0u
#define CFG_TRACK_BLACK_LEVEL   1u    /* 2026-11-20 实测�? = �?无反�?�? = 黑。取 1 表示需要取�?*/
/* 位序：模块返回的字节右移�?3 位（�?bsp_track.c 里已移回），
 * 还原�?X1 = bit7，这里反转一次把 X1 放到 bit0（最左），配合权重表使用�?*/
#define CFG_TRACK_ORDER_REVERSE 1u
/* PCB �?SCL/SDA 画反时的软件补救�? = 在代码里把两根线的角色对调�? * 默认 0（正常接法）。现场可用命令行 `s` 实时切换并自动探�?0x12�? * 哪个值能探测到就从这里改成对应值固化下来�?*/
#define CFG_TRACK_SWAP_SCL_SDA  0u
#define CFG_TRACK_I2C_HALF_US   20.0f /* 半周�?µs�?0 �?25 kHz�?                                      * 实测丢位数与速度成正比：
                                      *   200 kHz �?3 位�?0 kHz �?1 位�?5 kHz �?0 位�?                                      * 必须�?25 kHz 才能拿到完整�?8 位�?                                      * 代价是一次读取约 1.6 ms，所以循迹降�?100 Hz 读�?*/
#define CFG_TRACK_TIMEOUT_US    400u  /* 单次读取硬超时，防止拖死控制�?*/
#define CFG_TRACK_LOST_STOP_MS  300u  /* 丢线多久后停�?*/

/* 权重（单�?= 一个探头间距）。索�?0 = L1（最左）�? *
 * �?026-11-20 实测确认�? 路全部可用，L1�?x7F �?L8�?xFE，与手册完全一致�? * 前提：I2C 必须降到 50 kHz 以下�?00 kHz 时从机会丢前 3 个时钟，
 * 导致数据右移 3 位、看起来�?只有 5 �?）�?*/
#define CFG_TRACK_W0  (-3.5f)
#define CFG_TRACK_W1  (-2.5f)
#define CFG_TRACK_W2  (-1.5f)
#define CFG_TRACK_W3  (-0.5f)
#define CFG_TRACK_W4  ( 0.5f)
#define CFG_TRACK_W5  ( 1.5f)
#define CFG_TRACK_W6  ( 2.5f)
#define CFG_TRACK_W7  ( 3.5f)

/* ======================= IMU / HWT101 ======================= */
#define CFG_IMU_BAUD            115200u   /* [TODO] 必须�?9600 改上来，否则撑不�?100 Hz */
#define CFG_IMU_ANGLE_LSB       0.01f     /* [TODO] 角度�?LSB�?.01° */
#define CFG_IMU_GYRO_LSB        0.01f     /* [TODO] 角速度�?LSB�?.01°/s */
#define CFG_IMU_TIMEOUT_MS      200u
#define CFG_IMU_HOLDOVER_MS     300u      /* �?IMU 后靠上次 ω 维持多久 */

/* ======================= 通信 ======================= */
#define CFG_BT_BAUD             115200u
#define CFG_K230_BAUD           115200u
#define CFG_FAILSAFE_MS         500u      /* 上位机失�?*/

/* ======================= 电池 ======================= */
#define CFG_VBAT_R_TOP          47000.0f
#define CFG_VBAT_R_BOTTOM       10000.0f
#define CFG_VBAT_SCALE          ((CFG_VBAT_R_TOP + CFG_VBAT_R_BOTTOM) / CFG_VBAT_R_BOTTOM)
#define CFG_VBAT_WARN_MV        10500u
/* 低于这个值就认为"没接电池/分压没接"，不判低电压故障�? * 台架�?PA4 悬空�?ADC 读数是随机的，不设下限会误锁电机�?*/
#define CFG_VBAT_SENSE_MIN_MV   6000u
#define CFG_VBAT_STOP_MV        9900u

/* ======================= 速度规划 =======================
 * 上限�?JGB37-520 30:1 决定：空�?1.12 m/s，带载预�?0.7�?.9�? * 先按表里 ~70% 跑通，再逐级提；每次 +100 mm/s，连�?5 圈不失误才继续加�? */
#define CFG_V_STRAIGHT          300.0f /* first-run speed; raise to 800 after line following is verified */
#define CFG_V_CORNER            250.0f /* first-run corner speed */
#define CFG_V_SCURVE            600.0f
#define CFG_V_LOOP_CIRCLE       500.0f  /* 圆形环岛：半径大，可以快一�?*/
#define CFG_V_LOOP_HEX          400.0f  /* 六边形环岛：边短 + 60° 折角，必须减�?*/
#define CFG_V_BLIND             750.0f  /* 盲走区：别降太多，否则出口来不及拉回 */
#define CFG_V_RAMP              1200.0f /* mm/s²，速度指令的斜率限�?*/

/* ======================= 盲走区（地图底部断线�?======================= */
#define CFG_BLIND_ARM_MS        30u     /* 连续丢线多久判盲走（�? 个状态机周期�?*/
#define CFG_BLIND_MAX_MM        600     /* [TODO] 实测断口长度 + 200 余量，超预算就停�?*/

#endif /* APP_CONFIG_H */

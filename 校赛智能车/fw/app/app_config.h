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
#define CFG_WHEEL_DIAMETER_MM   67.08f /* 【2026-11-22 实测标定，覆盖上面的名义值 65】
                                        * 方法：地面上量 1000 mm，手推车（不按 a，IDLE 下里程照样累计），
                                        *       用 v 读 dist，取首尾差值。
                                        * 实测两次独立推车都是 969 mm（差 0 mm，可重复）→ 比例 0.969。
                                        * 修正：65.0 × 1000/969 = 67.08，使 dist 读数就是真实毫米。
                                        * 反过来说，这个 3.1% 的偏差也可能来自齿轮比（若 D 真是 65，
                                        * 则等效 CPR = 1279 ≈ 1:29 而不是 1:30）。两者效果完全相同，
                                        * 这里统一用轮径这一个旋钮，方便以后只用一次推车就能重标。
                                        * 影响面：dist 与测速都按 mpc 缩放 ⇒ 同样的速度指令实际车速比以前慢 3%，
                                        * 速度环不需要重调（它是闭环，跟着新比例自己收敛）。 */
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
#define CFG_STEER_KP            55.0f /* deg/s per pitch。2026-11-21 实测基线 55（配 kd=0、300/300）连续跑通；70 偏硬 */
#define CFG_STEER_KP_MAX        220.0f/* clamp for the steering term */
/* 2026-11-21 实测基线 0。8 路误差是 0.5 的台阶，kd=6 时微分被量化噪声打满
 * （一次换位 de=77 pitch/s → 463°/s，见 docs/09_corner_fix.md 缺陷①）。
 * 想重新启用 D，必须先按 09 文档第 4 节给 de 加低通，再用 0.3~1.0 的小值试。 */
#define CFG_STEER_KD            0.0f
/* 角速度环：dv = RATE_FF*w_ref + RATE_KP*(w_ref - w_meas) */
#define CFG_RATE_FF             (CFG_WHEEL_BASE_MM * 0.5f * 0.0174533f) /* mm/s per deg/s */
/* 内环增益。【实测 2026-11-22】0.35（环路增益 L=0.27）偏软，外环想提高就会摆；
 * 0.60（L=0.46）实测稳定：配合 yaw_kp=2.50，Z 1000 横向偏差仅 1.8 mm。 */
#define CFG_RATE_KP             0.60f
#define CFG_DV_MAX              420.0f/* 差速限幅 mm/s */
#define CFG_W_MAX               220.0f/* 期望角速度上限 deg/s */
/* 直道 / 盲走航向保持：w_ref += sgn*KYAW*(yaw_ref - yaw)
 * 【实测 2026-11-22】0.80 不够：纯 P 对恒定扰动必然有稳态误差。
 * 配合 yaw_hard（确定没线时禁止参考跟随）后，2.50 使 Z 1000 的 yaw 仅变 +0.2 度。 */
#define CFG_YAW_HOLD_KP         2.50f
#define CFG_YAW_HOLD_ERR_MAX    1.20f /* |e| 小于此值才启用航向保持（yaw_hard 时忽略） */

/* ======================= 直角弯（陀螺仪定角转弯） =======================
 * 90° 直角弯**物理上跟不了线**：线会横向离开 8 路视野（±35 mm），
 * 车再怎么打方向也看不见它。所以必须"按角度转"：
 *   yaw 误差 → w_ref = TURN_KP*dh → 角速度环 → 差速 → 速度环（与正常循迹同一条链路）
 * 转到位之后进 EL_SEEK 低速找线，锁线稳定再回 STRAIGHT。
 * 详见 docs/11_right_angle_and_blind.md */
/* ==== 航向符号：右转时 yaw 变大(+1) 还是变小(-1) ====
 * 全工程必须统一。历史上代码里是矛盾的：
 *   ctrl_core 的航向保持按「右转 yaw 变小」写，
 *   ctrl_fsm 的定角转弯按「右转 yaw 变大」写 —— 两者必有一错，
 *   错的那一路会变成正反馈，车朝一边跑飞（症状就是「走偏」）。
 * 2026-11-22 台架实测定案：**向右转 yaw 变小** → -1 正确。
 * 也就是说 ctrl_core 里原有的航向保持是对的，错的是新写的定角转弯（已统一）。
 * 换了模块的话用 Y 在线翻转即可，不用重编。 */
#define CFG_YAW_SGN            (-1.0f)

/* ======================= 补线（线消失时外推） =======================
 * 线消失时用最后几个有效点拟合出"误差随里程的斜率"，沿外推线继续打方向。
 * 一套逻辑同时处理断口和弯道（见 ctrl_core.c 第 5 节注释）。
 * 这是智能车竞赛的通用做法，比"按陀螺仪死转 90°"鲁棒得多：
 * 拐角角度不是 90°、或者转完偏移一两厘米，它都能自己找回来。
 */
#define CFG_PRED_HIST           5u    /* 用最近几个点算斜率 */
#define CFG_PRED_MIN_MM         6     /* 两点里程差小于这个值就不算斜率（太短不可信） */
#define CFG_PRED_MAX            4.0f  /* 外推误差上限（权重单位）。饱和 = 方向盘打满 */
#define CFG_PRED_TIMEOUT_MS     800u  /* 外推最多持续多久，超时归零交给航向保持 */
/* 是否还允许"按陀螺仪死转 90°"。补线成熟后建议关掉（0）。 */
#define CFG_TURN90_ENABLE       1u

/* 用"补线斜率"区分弯道和断口：|误差变化量 / 里程| 超过这个值 → 判弯道。
 * 单位是 权重单位/mm。直角弯约 0.1~0.3（线在一两厘米内横扫出视野），
 * 直道断口约 0.00~0.02。取 0.05 能分开两者。
 * 用斜率而不是"丢线前一刻的误差"，是因为后者随采样时刻随机跳
 * （实测同一个直角弯，err_lv 在 50~250 之间变）。 */
#define CFG_TURN_SLOPE_ARM      0.05f

/* 线丢失后"原地转到看见线"最多转这么多度。
 * 转满还看不到 → 判定为断口，改直行（见 ctrl_fsm.c 的 EL_TURN90）。
 * 取 110°：直角弯最多 100°（含斜切），留一点余量。 */
#define CFG_TURN_SCAN_MAX      130.0f

/* 断线区的自动判据：在 EL_STRAIGHT 里已经开了这么多毫米才丢线 → 直接进盲走，不转。
 * 0 = 关闭（每次都先转着找线）。
 * 依据：断线口在赛道底部那条长直道上，丢线前车已经在 STRAIGHT 里跑很久；
 *       直角弯通常紧跟在上一个元素之后，STRAIGHT 里的里程短。
 * 标定方法：跑一次直角弯、跑一次断口，看 [el] 日志里的 dist 差值。 */
/* 断线区判据：最后 5 个采样的平均 |误差| 小于这个值 → 判断口（直行，不转）。

   断线口线一直居中 ≈ 0；直角弯线在往外跑，最后两拍会把平均值拉到 0.5+。 */
#define CFG_GAP_MABS            0.35f

#define CFG_GAP_STRAIGHT_MM     0

/* ================= 断口里程窗（2026-11-22 晚新增，默认关闭）=================
 * 为什么要有它：直角弯和断口在 8 路上是同一件事（都是"丢线"），
 * 实测 err 区间重叠（弯 0.5~2.5 随机、口 0~0.5）、slope 都是 0，
 * 六种灰度判据 + 补线外推都试过，全部失败。断口位置固定，
 * 所以改用"自起跑以来的里程"分流 —— 这是第二个信息源，不依赖灰度。
 *
 * 单位 mm，原点是"第一次看到线"的那一刻（不是上电）。
 *
 * 标定方法（不用改代码，日志就够）：
 *   1. 起跑跑一次，把串口里所有 "[el] ... dist=" 的行抓出来
 *   2. 找 el=12 的两处：后面跟 13→2 的是直角弯，跟 13→10 的是断口
 *   3. 取断口那次的 dist，减去 el=2（第一次进直道）那次的 dist，得到 run
 *   4. D0 = run - 300，D1 = run + 300（断口 1.68 m，窗口开宽点容错）
 *
 * D1 <= D0 表示关闭（默认），行为与原来完全一致。 */
/* 【2026-11-22 结论：这两个数必须用"跑"的数据标定，因此默认关闭】
 *
 * 手推实测（不按 a，IDLE 下里程照样累计）：
 *   发车线 dist=0 / 断口起点 7235 / 断口终点 8717（长 1482，卷尺 1.5 m，差 1.2%）
 *   整圈回到发车线黑条 13061 — 与按图算的 13546 差 3.6%（地图/编码器互证通过）
 *
 * 但**跑起来的里程偏大**：实跑时 el 在 dist=7692 就已经进了盲走，而车还在断口
 * 之前没到。说明同一物理位置，跑动里程比手推大 6% 左右 —— 来源是 8 个直角弯
 * 的刹车相和原地转造成的轮子打滑累积。
 *
 * 所以手推的数**不能**直接填进窗口：窗口会提前 400~600 mm 触发，把断口前
 * 那个直角弯也圈进盲走区，而 EL_BLIND 里不会做 TURN90，车就直接乱走。
 *
 * 现场用串口 'G' 按跑的里程标定：
 *   G 0 0                        关窗，跑一圈
 *   车过断口（线刚消失）读 dist -> A；线重现 -> B
 *   G <A-250> <B-100>
 * 换赛道或改起点必须重标。D1 <= D0 表示关闭。 */
#define CFG_GAP_WIN_D0_MM       0
#define CFG_GAP_WIN_D1_MM       0

/* S-curve detection: disabled. It false-triggered on right-angle corners and

   its yaw_hold fought the extrapolation (measured: turn stopped at 29 deg).

   A plain follower handles S-curves anyway. Set back to 1 if needed. */
#define CFG_SCURVE_ENABLE       0u

/* 角度环是纯 P，靠"到位判据"停。停得准不准取决于三件事：
 *   1) K 太大      -> 冲过头
 *   2) 容差太松    -> 每次都差一点，连转四次累积成肉眼可见的偏
 *   3) 只看角度    -> 车还在高速转的时候就判定到位，停下时已经转过头
 * 2026-11-22：u 90 连转四次累积多转，就是 2) 和 3) 一起造成的。 */
#define CFG_TURN_KP            2.00f /* °/s per °。原 3.0 在 220 °/s 上限下常冲过头 */
#define CFG_TURN_DEG          105.0f  /* 故意多转 15°：转完车头略斜向新线，直行更容易撞到线 */
#define CFG_TURN_TOL_DEG        1.2f  /* 到位判据：|dh| 小于此值（原 2.0，四次累积到 8°） */
#define CFG_TURN_STOP_DPS      30.0f  /* 而且角速度要小于这个值才算真停住。
                                       * 少了这条：车以 200 °/s 冲过目标时 |dh| 也会短暂小于容差，
                                       * 判据立刻成立 -> 多转好几度。这是"多转一点"的直接原因。 */
#define CFG_TURN_SETTLE_MS      50u   /* 且要连续保持这么久才算到位（防抖） */
/* 进弯先刹停这么久，再开始转。让每次转弯都从 v≈0 起步，行为一致。

   实测：不加时转弯表现时好时坏（进弯 300 mm/s 要过渡到两轮 275/-195，

   过渡时间随 PID 状态和抓地变化）。 */
#define CFG_TURN_BRAKE_MS      220u
#define CFG_TURN_TIMEOUT_MS   3000u   /* 超时兜底：转不过来就放弃，绝不原地打转。
                                       * 2026-11-22 实测：赛道垫子上原地转的阻力远大于地板，
                                       * 1500 ms 只转得到 59°（平均 39°/s，运动学算的是 179°/s），
                                       * 于是被超时打断。先放到 3000 ms 验证。 */
#define CFG_TURN_ARM_ERR        1.20f /* 丢线前 |err| 超过这个值 → 判直角弯（不是断口、也不是能跟的弯）。
                                       * 2026-11-22 实测：直角弯丢线前 err_lv = 1.50；
                                       * 断口（盲走区）是车正对着缺口丢线，err_lv ≈ 0。
                                       * 两者隔得很开，取 1.2 正好分开。
                                       * 原值 2.80 是按"线贴到最边（3.5）才丢"估的，实测不是这样。 */
/* 环岛入口要求"线贴最边"持续这么久才认定。
 * 依据：直角弯也会让线贴到边上，但只持续一下下就整片消失；
 *       环岛是一直贴着、车跟着弯走。150 ms @300mm/s = 45 mm。 */
#define CFG_LOOP_ARM_MS        150u
#define CFG_TURN_FIND_ERR       3.20f /* 转角过程中只要重新看到线、且 |err| 小于此值，就认定已对正，
                                       * 提前结束转弯（不必转满 CFG_TURN_DEG）。
                                       * 2026-11-22 实测：转完 90° 后线刚好落在最外沿
                                       * （cnt=1, err=3.0），如果这里要求 1.5 就永远不满足，
                                       * 只能转满 90° 再靠 SEEK 撞运气。
                                       * 放宽到 3.2 = "只要看得见线就交回循迹"，
                                       * 剩下的偏差不多，循迹拉得回来。 */
/* 提前结束转弯必须"已经转过至少这么多度"。
 * 不加这个门槛的话，旋转过程中传感器一定会扫过原来那条线，
 * 那一瞬间 |err| 很小，会被误判成"已对正"。
 * 实测（2026-11-22）：直角弯只转了 58° 就提前退出，然后找线失败停车。
 *
 * 2026-11-22 第二次实测：设成 45° 时成功率只有约 60%，
 * 原因是在 45°~70° 之间传感器还可能扫到**旧线的残余**，
 * 于是提前退出、然后直行找不到新线。
 * 抬到 80° 后基本能转到位（原来 45° 相当于只转了大半就收）。
 * 代价：45° 斜切弯会转多（整张图上只有左上有一小段，先不管）。 */
#define CFG_TURN_EARLY_MIN_DEG  95.0f
#define CFG_V_TURN              40.0f /* 转弯时的前进速度。
                                       * 转弯时车身走的弧半径 R = v / ω。
                                       * 2026-11-22 实测：v=120 时转完 90° 横向偏移约 75 mm，
                                       * 已经超过 8 路总宽度（约 70 mm），转完完全看不到线，
                                       * SEEK 直行也找不回来（表现为"标准 90° 但脱线"）。
                                       * 降到 40 → R 约 13 mm，偏移落在传感器视野内，
                                       * 转完 SEEK 立刻能重新锁定，剩下的偏差交给循迹去修。 */
#define CFG_SEEK_TIMEOUT_MS    800u   /* 转完找线的超时，超了就停车 */
#define CFG_V_SEEK             180.0f /* 找线速度 */

#define CFG_ERR_LPF_HZ          20.0f
/* hold last line error this long after losing the line, so steering does not vanish in corners */
#define CFG_E_HOLD_MS           100u

/* ======================= 视觉 ======================= */
#define CFG_VIS_TIMEOUT_MS      300u
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
/* ==== 2026-11-22 修正：这两个 LSB 原来是拍脑袋的 0.01 ====
 * HWT101 走 WIT 标准协议（0x52 角速度帧 / 0x53 角度帧，int16 小端，和校验）：
 *   角度帧量程 ±180°      -> LSB = 180  / 32768 = 0.0054931640625 deg
 *   角速度帧量程 ±2000°/s -> LSB = 2000 / 32768 = 0.06103515625  deg/s
 * 原来写 0.01 的后果：
 *   1) yaw 被放大 1.82 倍：实际转 90°，车以为转了 164° -> 直角弯只转一半就收工
 *   2) 角速度被缩小 6.1 倍：角速度环反馈项几乎为零 -> 等于纯前馈，
 *      接了陀螺仪和没接一样
 * 验证：i 命令，手转 90°（直角器/A4 纸角卡住），读数应 ≈90。
 *   读到 ≈164 -> 还是旧值；读到 ≈45 -> 方向反了，改成 0.010986328125。 */
#define CFG_IMU_ANGLE_LSB       0.0054931640625f
#define CFG_IMU_GYRO_LSB        0.06103515625f
#define CFG_IMU_TIMEOUT_MS      200u
/* gyro sign: full project uses right-turn-positive. If turning the car RIGHT makes
 * the 'j' trace's 2nd number go NEGATIVE, set this to 1. Wrong sign makes the
 * rate loop positive-feedback and it will oscillate violently. */
#define CFG_GYRO_INVERT         0
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
#define CFG_V_STRAIGHT          300.0f /* 实测基线 300。参数台账见 docs/10_params_log.md */
#define CFG_V_CORNER            300.0f /* 实测基线：与直道同速。kp=55/kd=0 下弯道余量够用；提速时再拉开比值（目标 60~70%） */
/* 2026-11-21：元素状态机（S 弯 / 盲走 / 环岛）的判据会把循迹噪声当成元素特征，
 * 实测在直道上被误触发，目标速度直接从设定值跳到 600/750 —— 表现为「突然加速」。
 * 进来之后 kp_scale 还被降到 0.75 甚至 0（位置环断开），于是紧接着冲出赛道。
 * 这四个元素都还没上车验收，所以验收之前把速度压到不高于直道：
 * 误触发最多「不变速」，不会弹射。等元素标定完再逐个放开。
 * 详见 docs/09_corner_fix.md 第 8 节。 */
#define CFG_V_SCURVE            200.0f
#define CFG_V_LOOP_CIRCLE       200.0f  /* 圆形环岛：半径大，可以快一�?*/
#define CFG_V_LOOP_HEX          180.0f  /* 六边形环岛：边短 + 60° 折角，必须减�?*/
#define CFG_V_BLIND             200.0f  /* 盲走区：别降太多，否则出口来不及拉回 */
#define CFG_V_RAMP              1200.0f /* mm/s²，速度指令的斜率限�?*/

/* ======================= 盲走区（地图底部断线�?======================= */
#define CFG_BLIND_ARM_MS       120u     /* 丢线后等这么久才决定"转弯还是直行"。
                                         * 必须明显大于视觉的帧间隔（30Hz=33ms），
                                         * 否则视觉的"断口"还没报上来就已经决定转弯了。 */
#define CFG_BLIND_MAX_MM        1900     /* [TODO] 实测断口长度 + 200 余量，超预算就停�?*/

#endif /* APP_CONFIG_H */

/* ======================= 丢线后的策略 =======================
 * 1 = 【默认】丢线后状态机什么都不做，转向完全交给"补线"（误差外推）：
 *       断口 → 误差一直是 0 → 车头不动 → 直行
 *       直角弯 → 误差在增大 → 车一直转，转到看见线为止
 *     这是"不加判断、靠控制律自身性质分流"，断口处物理上不可能转弯。
 * 0 = 旧逻辑（丢线后按 err/斜率/视觉分类，转 105° 或进盲走）
 */
#define CFG_LOSS_AUTOPILOT      0

/* 丢线超过这么久还没重新看到线 → 停车（防止冲出赛道一直开）。
 * 断口 1.68 m、速度 200 mm/s 时需要约 8.4 s，所以这里给 10 s。 */
#define CFG_LOST_STOP_MS        10000u

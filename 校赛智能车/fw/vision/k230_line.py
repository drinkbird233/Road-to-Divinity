# -*- coding: utf-8 -*-
"""
K230 视觉辅助 —— 给 STM32 循迹车提供"前方是什么元素"

版本说明（重要）
----------------
这一版用的是 **get_regression（快速线性回归）**，也就是**实际跑通过**的那套：
实测 frames 正常递增、age 4~36 ms、angle 读数合理（竖线≈0）。

中间我试过用 find_blobs 量"黑块宽度"来判断直角弯，结果在板子上直接跑不起来，
已回退。现在这套是可靠的。

另外整套主循环都包了 try/except：**任何一帧出错都只打印一次原因，不会让程序死掉**，
而且仍然继续发帧（STM32 那边能看到心跳）。这样即使某个 API 在你的固件版本上
行为不同，也还能看到错误信息、还能继续联调。

发给 STM32 的内容（12 字节 payload）
------------------------------------
    [0]    valid      1 = 本帧有效
    [1..2] x          线在画面底部的横向偏差 -1000..1000（右正）
    [3..4] y          远带倾角 ×10（0.1°）—— 调试用
    [5..6] frame_id   帧号
    [7]    label      远带 x 像素 —— 调试用
    [8..9] angle      0.1°，近带线偏离竖直方向的角度（右倾为正，竖线=0）
    [10]   flags      0 直道 / 1 左直角 / 2 右直角 / 3 断口 / 4 环岛
                      bit3 = 本帧看到线
    [11]   quality    0..100 置信度

帧格式（和 STM32 端 proto.c 一致）
----------------------------------
    0xAA 0x55 | 0x10 | LEN | PAYLOAD | CRC8
    CRC8: poly 0x07, init 0x00，覆盖 TYPE + LEN + PAYLOAD

接线
----
    K230 TX → STM32 PC7 (USART6_RX)
    K230 RX → STM32 PC6 (USART6_TX)   ← 只发不收可以不接
    GND 共地
    波特率 115200 8N1
"""

import time
import math
from media.sensor import *
from media.display import *
from media.media import *

# ======================= 可调参数 =======================

WIDTH, HEIGHT = 160, 120       # 低分辨率换帧率。实测这一档 age 能到 4~36 ms
BLACK_THRESHOLD = (0, 90)      # 灰度阈值：黑线的亮度上限。现场用 IDE 调
UART_ID = 2                    # 只有退回标准 UART 时才用到
UART_BAUD = 115200

SHOW_DISPLAY = True            # 调参时开；跑车时关掉更快
SEND_PERIOD_MS = 20

# 两个观察带（按 HEIGHT 的比例）
BAND_NEAR_Y0 = 0.58            # 近处带（画面下部）
BAND_NEAR_H  = 0.36
BAND_FAR_Y0  = 0.10            # 远处带（画面上部）
BAND_FAR_H   = 0.32

TF_CORNER_DEG  = 30.0          # 远带倾角超过这个度数就判为直角弯（实测直道 -6°、右直角 +72°）
CENTER_TOL_PX  = 35            # 近带的线必须落在画面中心 ±这个像素内，否则认为是赛道边线
FAR_CENTER_TOL_PX = 55         # 远带的窗口放宽（直角弯横杆 xf≈49/107，边线在两侧）

# 版本号：每次改逻辑改一下，方便确认板子上跑的是哪一版
VERSION = "v2026-11-22c"

# ======================= 协议 =======================

PROTO_HEAD = b'\xAA\x55'
PROTO_T_VISION = 0x10

VIS_FLAG_NONE     = 0
VIS_FLAG_LEFT90   = 1
VIS_FLAG_RIGHT90  = 2
VIS_FLAG_GAP      = 3
VIS_FLAG_LOOP     = 4
VIS_FLAG_HAS_LINE = 0x08
VIS_FLAG_KIND_MASK = 0x07

# 连续帧确认：单帧抖动不算，连续 N 帧同一个 kind 才切换
KIND_HOLD_FRAMES = 3


def crc8(data):
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def i16(v):
    v = int(v) & 0xFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def build_frame(valid, x, y, fid, label, angle, flags, quality):
    pl = bytes([valid & 0xFF]) + i16(x) + i16(y) + i16(fid) + \
         bytes([label & 0xFF]) + i16(angle) + bytes([flags & 0xFF, quality & 0xFF])
    head = bytes([PROTO_T_VISION, len(pl)])
    return PROTO_HEAD + head + pl + bytes([crc8(head + pl)])


# ======================= 串口 =======================

def make_uart():
    """优先用板载的亚博库 YbUart（引脚已按这块板子配好）"""
    try:
        from ybUtils.YbUart import YbUart
        print("[uart] using YbUart")
        return YbUart(baudrate=UART_BAUD)
    except Exception as e:
        print("[uart] YbUart unavailable:", e)

    from machine import UART, FPIOA
    fpioa = FPIOA()
    fpioa.set_function(11, FPIOA.UART2_TXD)
    fpioa.set_function(12, FPIOA.UART2_RXD)
    return UART(UART_ID, baudrate=UART_BAUD, bits=UART.EIGHTBITS,
                parity=UART.PARITY_NONE, stop=UART.STOPBITS_ONE)


def uart_write(u, data):
    if hasattr(u, "send"):
        u.send(data)
    else:
        u.write(data)


# ======================= 图像处理 =======================

def regress_band(img, y0_ratio, h_ratio):
    """
    在一条水平带里做线性回归。
    返回 (theta_deg, x_bottom, magnitude) 或 None
      theta_deg : 线偏离竖直方向的角度，右倾为正（竖线 = 0）
    """
    y0 = int(HEIGHT * y0_ratio)
    h = int(HEIGHT * h_ratio)
    if y0 + h > HEIGHT:
        h = HEIGHT - y0
    if h < 8:
        return None

    line = img.get_regression([BLACK_THRESHOLD], roi=(0, y0, WIDTH, h), robust=True)
    if not line:
        return None

    x1, y1, x2, y2 = line.line()

    # 关键：先按 y 排好端点的上下顺序，再用 atan2(dx, dy)。
    # 不排序的话，端点顺序一变，角度就会在 0° 和 180° 之间跳（实测出现 154.9°）。
    if y1 < y2:
        xt, yt, xb, yb = x1, y1, x2, y2
    else:
        xt, yt, xb, yb = x2, y2, x1, y1

    dx = xt - xb
    dy = yb - yt
    if dy < 1:
        dy = 1
    theta = math.degrees(math.atan2(dx, dy))
    return (theta, xb, line.magnitude())


def analyse(img):
    """返回 (valid, x_norm, angle_dd, flags, quality, dbg)

    【2026-11-22 重要修正】
    原来用"线在画面哪一边"（绝对位置 xn/xf 和倾角符号）判断左右，
    但摄像头装偏时"线永远在同一边"，于是永远报同一个方向。
    现在改成**只看远近两带的差值**：

        dx_bands = xf - xn        ← 摄像头整体平移时两个带一起平移，差值不变

    差值只反映"线的形状在拐没拐"，与摄像头装在哪无关。
    """
    near = regress_band(img, BAND_NEAR_Y0, BAND_NEAR_H)
    far = regress_band(img, BAND_FAR_Y0, BAND_FAR_H)

    # 【关键过滤】盲走区没有中线，但赛道**边线**还在画面里，回归会把边线当成线，
    # 于是盲走区被误报成左/右直角。实测确认过这个现象。
    # 近带的线必须落在画面中心附近，否则不认（那多半是边线）。
    if near is not None and abs(near[1] - WIDTH / 2.0) > CENTER_TOL_PX:
        near = None

    # 远带也要过滤，但窗口放宽 —— 远处本来就可以偏，
    # 直角弯的横杆实测在 xf=49（右）和 xf=107（左），都能落在 ±55 以内；
    # 而赛道的边线在画面两侧（x<25 或 x>135），会被挡掉。
    if far is not None and abs(far[1] - WIDTH / 2.0) > FAR_CENTER_TOL_PX:
        far = None

    tn = near[0] if near else None
    tf = far[0] if far else None
    xn = near[1] if near else None
    xf = far[1] if far else None
    dbg = (tn, tf, xn, xf)

    if near is None and far is None:
        return (1, 0, 0, VIS_FLAG_GAP, 0, dbg)

    if near is None and far is not None:
        # 近处没有、远处有 → 线在车下断掉了
        return (1, 0, 0, VIS_FLAG_GAP, 20, dbg)

    theta, xb, mag = near
    x_norm = int((xb - WIDTH / 2.0) / (WIDTH / 2.0) * 1000.0)
    if x_norm > 1000:
        x_norm = 1000
    if x_norm < -1000:
        x_norm = -1000

    flags = VIS_FLAG_HAS_LINE

    # 【2026-11-22 用实测数据定下来的规则】
    # 同一位置连测 4 次，读数完全一致（非常稳定）：
    #   直道   tf = -6.1°           → LINE
    #   右直角 tf = +66.7°          → RIGHT90
    #   左直角 tf = -39 ~ -74°      → LEFT90
    #   断口   远带完全没有线        → GAP
    # 所以直接看**远带倾角 tf 的符号和大小**，比"远近两带的差"区分度高得多。
    if far is None:
        # 远带没线：
        #   · 两个带都没线  → 真的断口
        #   · 只有远带没线  → 可能是反光/抖动，也可能线在前面断了。
        #     这里**保守处理成 LINE**（不乱给方向），等车再往前一点两个带都没线
        #     再判 GAP。实测直道偶尔会有一帧远带丢线，这样不会误报断口。
        if near is None:
            flags |= VIS_FLAG_GAP
    elif tf >= TF_CORNER_DEG:
        flags |= VIS_FLAG_RIGHT90
    elif tf <= -TF_CORNER_DEG:
        flags |= VIS_FLAG_LEFT90

    q = 100 if (flags & VIS_FLAG_KIND_MASK) == VIS_FLAG_NONE else 80
    return (1, x_norm, int(theta * 10.0), flags, q, dbg)


# ======================= 主循环 =======================

def main():
    print("==== K230 vision %s ====" % VERSION)
    sensor = None
    uart = None
    try:
        sensor = Sensor()
        sensor.reset()
        sensor.set_framesize(width=WIDTH, height=HEIGHT)
        sensor.set_pixformat(Sensor.GRAYSCALE)

        if SHOW_DISPLAY:
            try:
                Display.init(Display.ST7701, to_ide=True)
            except Exception as e:
                print("[display] init failed (继续跑):", e)

        MediaManager.init()
        sensor.run()

        uart = make_uart()
        clock = time.clock()
        fid = 0
        next_ms = time.ticks_ms()
        last_print = time.ticks_ms()
        frames_since = 0
        err_shown = 0
        kind_hold = 0            # 正在确认的 kind
        kind_cnt = 0             # 它连续出现了几帧
        kind_out = 0             # 已确认的 kind（发出去用的）

        while True:
            clock.tick()
            img = sensor.snapshot()

            try:
                valid, x_norm, angle_dd, flags, q, dbg = analyse(img)
            except Exception as e:
                # 单帧出错不能让程序死掉：打印一次原因，继续发心跳帧
                if err_shown < 3:
                    err_shown += 1
                    print("[analyse error]", e)
                valid, x_norm, angle_dd, flags, q = 0, 0, 0, 0, 0
                dbg = (None, None, None, None)

            fid = (fid + 1) & 0xFFFF
            frames_since += 1

            tn, tf, xn, xf = dbg

            # ---- 连续帧确认 ----
            # 单帧判出来的 kind 先不认，连续 KIND_HOLD_FRAMES 帧都一样才采纳。
            # 这样能干掉画面抖动导致的瞬间误报（"直线突然报右转"那种）。
            raw_kind = flags & VIS_FLAG_KIND_MASK
            if raw_kind == kind_hold:
                kind_cnt += 1
            else:
                kind_hold = raw_kind
                kind_cnt = 1
            if kind_cnt >= KIND_HOLD_FRAMES:
                kind_out = kind_hold
            # 未达确认次数前，沿用上一次确认过的 kind
            flags = (flags & ~VIS_FLAG_KIND_MASK) | kind_out

            y_val  = int(tf * 10.0) if tf is not None else 0
            lab_xf = (int(xf) & 0xFF) if xf is not None else 0

            try:
                uart_write(uart, build_frame(valid, x_norm, y_val, fid, lab_xf,
                                             angle_dd, flags, q))
            except Exception as e:
                if err_shown < 3:
                    err_shown += 1
                    print("[uart error]", e)

            if SHOW_DISPLAY:
                try:
                    img.draw_line(WIDTH // 2, 0, WIDTH // 2, HEIGHT, color=128)
                    Display.show_image(img)
                except Exception:
                    pass

            now = time.ticks_ms()
            if time.ticks_diff(now, last_print) > 500:
                dt = time.ticks_diff(now, last_print)
                fps = frames_since * 1000.0 / dt
                print("[k230] %s " % VERSION + "fps=%.1f x=%d ang=%.1f kind=%d | tn=%s tf=%s xn=%s xf=%s" % (
                    fps, x_norm, angle_dd / 10.0, flags & 0x07,
                    ("%.1f" % tn) if tn is not None else "--",
                    ("%.1f" % tf) if tf is not None else "--",
                    ("%d" % xn) if xn is not None else "--",
                    ("%d" % xf) if xf is not None else "--"))
                last_print = now
                frames_since = 0

            while time.ticks_diff(time.ticks_ms(), next_ms) < 0:
                pass
            next_ms = time.ticks_add(next_ms, SEND_PERIOD_MS)

    except KeyboardInterrupt:
        print("stopped")
    except Exception as e:
        print("fatal:", e)
    finally:
        try:
            if sensor is not None:
                sensor.stop()
            Display.deinit()
            MediaManager.deinit()
        except Exception:
            pass


if __name__ == "__main__":
    main()

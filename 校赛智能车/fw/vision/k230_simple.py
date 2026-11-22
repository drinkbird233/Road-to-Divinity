# -*- coding: utf-8 -*-
"""
K230 视觉辅助 —— 极简版

只做一件简单的事：
    看画面【中间那条竖带】里有没有黑线
        有  → LINE
        没有 → GAP（盲走区）

为什么这么简单就够了
--------------------
8 路灰度只能看车底 7 cm，到了弯口/断口才知道。摄像头能看前方 30~50 cm，
所以它唯一不可替代的价值就是：**提前知道前面还有没有线**。

至于"左转还是右转"，灰度传感器本来就更擅长（它直接看线的左右位置），
不需要摄像头来判断 —— 之前那些回归、倾角、远近对比，全是把简单问题做复杂了。

关键设计：只看中间 40% 的宽度
------------------------------
赛道两侧有边线。如果整幅图都看，边线会被当成中线，盲走区就会被误判成"有线"。
**只看中间 40%** 之后，边线落在视野外，剩下的只有真正的中线。

发给 STM32 的帧（12 字节，和原来一致，STM32 端不用改）
-------------------------------------------------------
    [0]    valid
    [1..2] x          中线在画面里的横向偏差 -1000..1000（右正）
    [3..4] y          0（保留）
    [5..6] frame_id
    [7]    label      0
    [8..9] angle      0
    [10]   flags      0 有线 / 3 断口；bit3 = 有线
    [11]   quality    0..100

接线：K230 TX → STM32 PC7，GND 共地，115200
"""

import time
from media.sensor import *
from media.display import *
from media.media import *

VERSION = "simple-v1"

# ==================== 参数 ====================

WIDTH, HEIGHT = 160, 120

BLACK_THRESHOLD = (0, 90)     # 黑线亮度上限。现场用 IDE 调
STRIP_X0 = 0.30               # 只看中间这条竖带：30% ~ 70% 宽
STRIP_X1 = 0.70

NEAR_Y0 = 0.55                # 近处：画面 55%~95% 高（离车最近，判断"还有没有线"）
NEAR_Y1 = 0.95
FAR_Y0 = 0.12                 # 远处：12%~50% 高（提前量，暂时只做记录）
FAR_Y1 = 0.50

MIN_BLACK = 40                # 一个区域里至少要有这么多黑像素才算"有线"
                              # 160×64 的带子里一条线约占 100~200 像素

SHOW_DISPLAY = True
UART_BAUD = 115200

# ==================== 协议 ====================

PROTO_HEAD = b'\xAA\x55'
PROTO_T_VISION = 0x10
VIS_FLAG_HAS_LINE = 0x08
VIS_FLAG_GAP = 3


def crc8(data):
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def i16(v):
    v = int(v) & 0xFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def build_frame(x, fid, flags, quality):
    pl = bytes([1]) + i16(x) + i16(0) + i16(fid) + bytes([0]) + \
         i16(0) + bytes([flags & 0xFF, quality & 0xFF])
    head = bytes([PROTO_T_VISION, len(pl)])
    return PROTO_HEAD + head + pl + bytes([crc8(head + pl)])


# ==================== 串口 ====================

def make_uart():
    try:
        from ybUtils.YbUart import YbUart
        print("[uart] YbUart")
        return YbUart(baudrate=UART_BAUD)
    except Exception as e:
        print("[uart] YbUart no:", e)
    from machine import UART, FPIOA
    fpioa = FPIOA()
    fpioa.set_function(11, FPIOA.UART2_TXD)
    fpioa.set_function(12, FPIOA.UART2_RXD)
    return UART(2, baudrate=UART_BAUD)


def uart_write(u, d):
    if hasattr(u, "send"):
        u.send(d)
    else:
        u.write(d)


# ==================== 核心：中间带里有没有黑 ====================

def count_black(img, y0_ratio, y1_ratio):
    """返回 (黑像素数, 黑块中心x) —— 只看中间 40% 宽的竖带"""
    x0 = int(WIDTH * STRIP_X0)
    x1 = int(WIDTH * STRIP_X1)
    y0 = int(HEIGHT * y0_ratio)
    y1 = int(HEIGHT * y1_ratio)
    if y1 <= y0 or x1 <= x0:
        return (0, WIDTH // 2)

    blobs = img.find_blobs([BLACK_THRESHOLD],
                           roi=(x0, y0, x1 - x0, y1 - y0),
                           area_threshold=10, merge=True)
    if not blobs:
        return (0, WIDTH // 2)

    n = 0
    wx = 0
    for b in blobs:
        n += b[4]               # pixels
        wx += b[5] * b[4]       # cx * pixels
    return (n, wx // n if n else WIDTH // 2)


# ==================== 主循环 ====================

def main():
    print("==== K230 vision %s ====" % VERSION)
    sensor = None
    try:
        sensor = Sensor()
        sensor.reset()
        sensor.set_framesize(width=WIDTH, height=HEIGHT)
        sensor.set_pixformat(Sensor.GRAYSCALE)

        if SHOW_DISPLAY:
            try:
                Display.init(Display.ST7701, to_ide=True)
            except Exception as e:
                print("[display] no:", e)
        MediaManager.init()
        sensor.run()

        uart = make_uart()
        fid = 0
        last_print = time.ticks_ms()
        frames_since = 0
        next_ms = time.ticks_ms()

        while True:
            img = sensor.snapshot()

            n_near, cx_near = count_black(img, NEAR_Y0, NEAR_Y1)
            n_far, _ = count_black(img, FAR_Y0, FAR_Y1)

            has_line = n_near >= MIN_BLACK

            if has_line:
                flags = VIS_FLAG_HAS_LINE           # 0x08
                # 横向偏差：中线相对画面中心的偏移，-1000..1000
                x = int((cx_near - WIDTH / 2.0) / (WIDTH / 2.0) * 1000.0)
                x = max(-1000, min(1000, x))
                q = 100
            else:
                flags = VIS_FLAG_GAP                 # 3
                x = 0
                q = 0

            fid = (fid + 1) & 0xFFFF
            frames_since += 1
            uart_write(uart, build_frame(x, fid, flags, q))

            if SHOW_DISPLAY:
                try:
                    x0 = int(WIDTH * STRIP_X0)
                    x1 = int(WIDTH * STRIP_X1)
                    img.draw_rectangle(x0, int(HEIGHT * NEAR_Y0), x1 - x0,
                                       int(HEIGHT * (NEAR_Y1 - NEAR_Y0)), color=255)
                    Display.show_image(img)
                except Exception:
                    pass

            now = time.ticks_ms()
            if time.ticks_diff(now, last_print) > 500:
                fps = frames_since * 1000.0 / (time.ticks_diff(now, last_print) / 1000.0)
                print("[k230] %s fps=%.1f near=%d far=%d %s" % (
                    VERSION, fps, n_near, n_far, "LINE" if has_line else "GAP"))
                last_print = now
                frames_since = 0

            while time.ticks_diff(time.ticks_ms(), next_ms) < 0:
                pass
            next_ms = time.ticks_add(next_ms, 20)

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

# -*- coding: utf-8 -*-
"""
K230 串口心跳测试 —— 用来把"串口通不通"和"视觉代码对不对"分开

它不打开摄像头、不打开显示，只每 100 ms 发一帧固定内容的帧。
如果 STM32 发 K（或 vi）能看到 frames 一直涨，说明：
    ✅ 串口线对、波特率对、YbUart/machine.UART 能用、协议对
    ❌ 那 k230_line.py 跑不起来就是**视觉代码**的问题（摄像头/查找参数）
如果 frames 不涨，那问题在硬件或串口配置，跟视觉无关。

用法：把这个文件在 CanMV IDE 里运行，然后 STM32 上发 K 看 frames。
"""

import time

# ---------------- 协议 ----------------

PROTO_HEAD = b'\xAA\x55'
PROTO_T_VISION = 0x10


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


# ---------------- 串口 ----------------

def make_uart():
    try:
        from ybUtils.YbUart import YbUart
        print("[uart] YbUart ok")
        return YbUart(baudrate=115200)
    except Exception as e:
        print("[uart] YbUart unavailable:", e)

    try:
        from machine import UART, FPIOA
        fpioa = FPIOA()
        # ⚠️ 按板子丝印改
        fpioa.set_function(11, FPIOA.UART2_TXD)
        fpioa.set_function(12, FPIOA.UART2_RXD)
        u = UART(2, baudrate=115200)
        print("[uart] machine.UART ok")
        return u
    except Exception as e:
        print("[uart] machine.UART failed:", e)
        return None


def send(u, data):
    if hasattr(u, "send"):
        u.send(data)
    else:
        u.write(data)


# ---------------- 主循环 ----------------

def main():
    uart = make_uart()
    if uart is None:
        print("!! no uart, stop")
        return

    n = 0
    try:
        while True:
            n = (n + 1) & 0xFFFF
            # flags = 0x08（HAS_LINE）+ 0（直道），x 跟着变，方便看数值是否在更新
            f = build_frame(1, (n % 200) - 100, 0, n, 0, 0, 0x08, 100)
            send(uart, f)
            if n % 10 == 0:
                print("sent", n, "bytes", len(f))
            time.sleep_ms(100)
    except KeyboardInterrupt:
        print("stopped")


main()

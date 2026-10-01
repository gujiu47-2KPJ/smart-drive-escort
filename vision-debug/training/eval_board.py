#!/usr/bin/env python3
"""评估板子当前状态：读 COM5，判断固件、帧率、单帧大小。"""

import time

import serial

PORT = "COM5"
BAUD = 921600
MAGIC = bytes([0xAA, 0xBB])
SOI = bytes([0xFF, 0xD8])


def count_valid_frames(buf):
    """严格解析：0xAA 0xBB + 2字节长度 + 负载以 0xFFD8 开头才算一帧。"""
    n = 0
    sizes = []
    i = 0
    L = len(buf)
    while i < L - 1:
        if buf[i] == MAGIC[0] and buf[i + 1] == MAGIC[1]:
            if i + 6 <= L:
                ln = buf[i + 2] | (buf[i + 3] << 8)
                if buf[i + 4:i + 6] == SOI and 100 < ln < 200000:
                    n += 1
                    sizes.append(ln)
                    i += 4 + ln
                    continue
            i += 1
        else:
            i += 1
    return n, sizes


def main():
    ser = serial.Serial(PORT, BAUD, timeout=1)
    ser.setDTR(False)
    time.sleep(0.2)
    ser.setDTR(True)
    time.sleep(2.5)          # 等板子启动 + 切波特率

    buf = bytearray()
    t0 = time.time()
    DUR = 5.0
    while time.time() - t0 < DUR:
        d = ser.read(4096)
        if d:
            buf.extend(d)

    n, sizes = count_valid_frames(buf)
    print(f"bytes={len(buf)}  valid_frames={n}  fps={n / DUR:.1f}")
    if sizes:
        avg = sum(sizes) / len(sizes)
        print(f"frame_size min={min(sizes)} avg={int(avg)} max={max(sizes)}")
    ser.close()


if __name__ == "__main__":
    main()

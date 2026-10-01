#!/usr/bin/env python3
"""
智行护航 - 串口摄像头实时查看器

读取 ESP32-S3 从 USB 串口(COM5)推出来的 JPEG 帧，并用 OpenCV 窗口显示。

用法：
    python serial_viewer.py                # 默认 COM5 @ 921600
    python serial_viewer.py --port COM9 --baud 115200

依赖（本机已装好）：
    pip install pyserial opencv-python numpy
"""

import argparse
import sys

import cv2
import numpy as np
import serial

FRAME_MAGIC0 = 0xAA
FRAME_MAGIC1 = 0xBB
MAX_FRAME_LEN = 200 * 1024  # 单帧 JPEG 上限保护


def read_frame(ser: serial.Serial):
    """从串口读取一帧 JPEG（返回 bytes 或 None）。"""
    # 1) 等待帧头 0xAA 0xBB
    while True:
        b = ser.read(1)
        if not b:
            return None
        if b[0] == FRAME_MAGIC0:
            b2 = ser.read(1)
            if b2 and b2[0] == FRAME_MAGIC1:
                break

    # 2) 读取 2 字节小端长度
    lb = ser.read(2)
    if len(lb) < 2:
        return None
    length = lb[0] | (lb[1] << 8)
    if length == 0 or length > MAX_FRAME_LEN:
        return None

    # 3) 读取 JPEG 数据
    payload = bytearray()
    while len(payload) < length:
        chunk = ser.read(length - len(payload))
        if not chunk:
            return None
        payload.extend(chunk)
    return bytes(payload)


def main():
    ap = argparse.ArgumentParser(description="ESP32-S3 串口摄像头查看器")
    ap.add_argument("--port", default="COM5", help="串口号，默认 COM5")
    ap.add_argument("--baud", type=int, default=921600, help="波特率，默认 921600")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:
        print(f"无法打开串口 {args.port}: {e}")
        sys.exit(1)

    print(f"正在监听 {args.port} @ {args.baud} ...")
    print("按 'q' 退出，按 's' 保存当前画面为 snapshot.jpg")

    win = "ESP32-S3 camera"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)

    try:
        while True:
            jpeg = read_frame(ser)
            if jpeg is None:
                # 串口空闲，检查是否有按键
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
                continue

            img = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                continue

            cv2.imshow(win, img)
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            if key == ord("s"):
                cv2.imwrite("snapshot.jpg", img)
                print("已保存 snapshot.jpg")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        cv2.destroyAllWindows()
        print("已退出")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""验证 ESP32-S3 串口摄像头推流是否正常（查找帧头魔数 + JPEG 起始标记）。"""
import sys, time
import serial

PORT = "COM5"
BAUD = 921600
MAGIC = bytes([0xAA, 0xBB])
SOI = bytes([0xFF, 0xD8])

def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
    # 复位板子（拉低 DTR）
    ser.setDTR(False)
    time.sleep(0.2)
    ser.setDTR(True)
    time.sleep(2.5)  # 等待启动 + 切换波特率

    buf = bytearray()
    t_end = time.time() + 5.0
    magic_count = 0
    jpeg_count = 0
    while time.time() < t_end:
        data = ser.read(4096)
        if data:
            buf.extend(data)

    total = len(buf)
    magic_count = buf.count(MAGIC)
    # 检查 0xAA 0xBB 后是否紧跟 0xFF 0xD8（JPEG SOI）
    i = buf.find(MAGIC)
    while i >= 0:
        if i + 4 + 2 <= len(buf) and buf[i+4:i+6] == SOI:
            jpeg_count += 1
        i = buf.find(MAGIC, i + 1)

    print(f"total_bytes={total}")
    print(f"magic_count(0xAA 0xBB)={magic_count}")
    print(f"jpeg_frames(followed by 0xFFD8)={jpeg_count}")
    if total > 0:
        print(f"first_16_bytes={buf[:16].hex(' ')}")
        # 找到第一帧 JPEG 并保存
        idx = buf.find(MAGIC)
        if idx >= 0 and idx + 4 < len(buf):
            length = buf[idx+2] | (buf[idx+3] << 8)
            payload = bytes(buf[idx+4:idx+4+length])
            if payload.startswith(SOI):
                with open("verify_frame.jpg", "wb") as f:
                    f.write(payload)
                print(f"saved verify_frame.jpg ({length} bytes)")

    ser.close()

if __name__ == "__main__":
    main()

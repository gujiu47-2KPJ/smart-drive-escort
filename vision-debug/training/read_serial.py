#!/usr/bin/env python3
"""读串口指定时长并打印（用于观察板上日志）。"""

import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0

ser = serial.Serial(port, baud, timeout=1)
ser.setDTR(False)
time.sleep(0.1)
ser.setDTR(True)
ser.reset_input_buffer()

t0 = time.time()
buf = bytearray()
while time.time() - t0 < dur:
    d = ser.read(4096)
    if d:
        buf.extend(d)
ser.close()

text = buf.decode("utf-8", errors="replace")
print(text)

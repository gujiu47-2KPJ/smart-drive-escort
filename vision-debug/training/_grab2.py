"""连续抓 6 帧，画出检测框并保存，用于核对横纹与框位置。"""

import os
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from person_viewer import read_frame, letterbox, postprocess  # noqa: E402

sess = ort.InferenceSession(os.path.join(HERE, "models", "person_320.onnx"),
                            providers=["CPUExecutionProvider"])
iname = sess.get_inputs()[0].name

ser = serial.Serial("COM5", 921600, timeout=1)
ser.setDTR(False)
time.sleep(0.2)
ser.setDTR(True)
time.sleep(2.5)
ser.reset_input_buffer()

outdir = os.path.join(HERE, "_grabs2")
os.makedirs(outdir, exist_ok=True)
for f in os.listdir(outdir):
    os.remove(os.path.join(outdir, f))

print("capturing 6 frames over ~6s ...")
for i in range(6):
    t_end = time.time() + 1.0
    frame = None
    while time.time() < t_end:
        j = read_frame(ser)
        if j:
            frame = cv2.imdecode(np.frombuffer(j, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is not None:
                break
    if frame is None:
        print(i, "no frame")
        continue
    lb, r, (left, top) = letterbox(frame, (320, 320))
    blob = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    blob = blob.transpose(2, 0, 1)[None]
    out = sess.run(None, {iname: blob})[0]
    boxes, scores = postprocess(out, 0.3, 0.5)
    vis = frame.copy()
    print("frame %d: max=%.3f dets=%d" % (i, out[0][4, :].max(), len(boxes)))
    for (x1, y1, x2, y2), sc in zip(boxes, scores):
        bx1 = int(max((x1 - left) / r, 0))
        by1 = int(max((y1 - top) / r, 0))
        bx2 = int(min((x2 - left) / r, vis.shape[1] - 1))
        by2 = int(min((y2 - top) / r, vis.shape[0] - 1))
        print("   person %.2f box=(%d,%d,%d,%d)" % (sc, bx1, by1, bx2, by2))
        cv2.rectangle(vis, (bx1, by1), (bx2, by2), (0, 255, 0), 1)
        cv2.putText(vis, "%.2f" % sc, (bx1, max(by1 - 4, 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 0), 1)
    cv2.imwrite(os.path.join(outdir, "box_%d.jpg" % i), vis)

ser.close()
print("saved to", outdir)

"""连续抓 8 帧（每约 1 秒一帧），保存并逐帧跑模型，记录分数。"""

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

outdir = os.path.join(HERE, "_grabs")
os.makedirs(outdir, exist_ok=True)
for f in os.listdir(outdir):
    os.remove(os.path.join(outdir, f))

print("capturing 8 frames over ~8s ...")
for i in range(8):
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
    lb, r, (l, t) = letterbox(frame, (320, 320))
    blob = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    blob = blob.transpose(2, 0, 1)[None]
    out = sess.run(None, {iname: blob})[0]
    sc = out[0][4, :]
    boxes, scores = postprocess(out, 0.3, 0.5)
    name = "_grab_%d_score%.3f_n%d.jpg" % (i, sc.max(), len(boxes))
    cv2.imwrite(os.path.join(outdir, name), frame)
    print("frame %d: max_score=%.3f  top3=%s  dets(0.3)=%d  -> %s"
          % (i, sc.max(), np.round(np.sort(sc)[-3:], 3).tolist(), len(boxes), name))

ser.close()
print("done")

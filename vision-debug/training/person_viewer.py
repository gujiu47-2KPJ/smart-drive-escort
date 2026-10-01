#!/usr/bin/env python3
"""
智行护航 - 串口推流 + 自训练单类(person)模型 实时检测查看器

板子：serial-camera 固件（QVGA JPEG @ 921600）
电脑：读串口帧 -> onnxruntime 跑 person_320.onnx -> 画框显示

用法：
    python person_viewer.py
    python person_viewer.py --conf 0.4 --scale 2
"""

import argparse
import os
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort
import serial

FRAME_MAGIC0 = 0xAA
FRAME_MAGIC1 = 0xBB
MAX_FRAME_LEN = 200 * 1024

CLASSES = ["person"]


def read_frame(ser):
    """从串口读取一帧 JPEG（bytes 或 None）。"""
    while True:
        b = ser.read(1)
        if not b:
            return None
        if b[0] == FRAME_MAGIC0:
            b2 = ser.read(1)
            if b2 and b2[0] == FRAME_MAGIC1:
                break
    lb = ser.read(2)
    if len(lb) < 2:
        return None
    length = lb[0] | (lb[1] << 8)
    if length == 0 or length > MAX_FRAME_LEN:
        return None
    payload = bytearray()
    while len(payload) < length:
        chunk = ser.read(length - len(payload))
        if not chunk:
            return None
        payload.extend(chunk)
    return bytes(payload)


def letterbox(im, new_shape=(320, 320), color=(114, 114, 114)):
    """等比缩放 + 灰边填充到 new_shape，返回 (图, 比例, (left, top))。"""
    shape = im.shape[:2]                      # h, w
    r = min(new_shape[0] / shape[0], new_shape[1] / shape[1])
    new_unpad = (int(round(shape[1] * r)), int(round(shape[0] * r)))
    dw = (new_shape[1] - new_unpad[0]) / 2.0
    dh = (new_shape[0] - new_unpad[1]) / 2.0
    if shape[::-1] != new_unpad:
        im = cv2.resize(im, new_unpad, interpolation=cv2.INTER_LINEAR)
    top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
    left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
    im = cv2.copyMakeBorder(im, top, bottom, left, right, cv2.BORDER_CONSTANT, value=color)
    return im, r, (left, top)


def postprocess(out, conf_thres, iou_thres):
    """YOLOv8 输出 [1, 4+nc, N] -> NMS 后的 xyxy 框。"""
    pred = out[0]                      # [5, 2100]
    pred = pred.T                      # [2100, 5]
    scores = pred[:, 4]
    keep = scores > conf_thres
    if not np.any(keep):
        return np.empty((0, 4)), np.empty((0,))
    pred = pred[keep]
    scores = scores[keep]

    # cxcywh -> xyxy
    boxes = np.empty_like(pred[:, :4])
    boxes[:, 0] = pred[:, 0] - pred[:, 2] / 2.0
    boxes[:, 1] = pred[:, 1] - pred[:, 3] / 2.0
    boxes[:, 2] = pred[:, 0] + pred[:, 2] / 2.0
    boxes[:, 3] = pred[:, 1] + pred[:, 3] / 2.0

    idx = cv2.dnn.NMSBoxes(boxes.tolist(), scores.tolist(), conf_thres, iou_thres)
    if len(idx) == 0:
        return np.empty((0, 4)), np.empty((0,))
    idx = np.array(idx).reshape(-1)
    return boxes[idx], scores[idx]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="串口推流 + 自训练 person 模型")
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--model", default=os.path.join(here, "models", "person_320.onnx"))
    ap.add_argument("--conf", type=float, default=0.4)
    ap.add_argument("--iou", type=float, default=0.5)
    ap.add_argument("--scale", type=float, default=2.0)
    args = ap.parse_args()

    if not os.path.exists(args.model):
        print("找不到模型:", args.model)
        sys.exit(1)
    sess = ort.InferenceSession(args.model, providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name
    print("模型就绪:", os.path.basename(args.model))

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:
        print("无法打开串口:", e)
        sys.exit(1)

    print(f"监听 {args.port} @ {args.baud} ... 按 q 退出，s 存图，p 暂停检测")
    win = "ESP32-S3 person (self-trained)"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)

    detect_on = True
    last_boxes, last_scores = np.empty((0, 4)), np.empty((0,))
    infer_ms = 0.0
    fps_t0, fps_n, fps = time.time(), 0, 0.0

    try:
        while True:
            jpeg = read_frame(ser)
            if jpeg is None:
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
                continue
            frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                continue

            if detect_on:
                lb, r, (left, top) = letterbox(frame, (320, 320))
                blob = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
                blob = blob.transpose(2, 0, 1)[None]
                t0 = time.time()
                out = sess.run(None, {in_name: blob})[0]
                infer_ms = (time.time() - t0) * 1000.0
                boxes, scores = postprocess(out, args.conf, args.iou)
                # 映射回原图坐标
                if len(boxes):
                    boxes[:, [0, 2]] = (boxes[:, [0, 2]] - left) / r
                    boxes[:, [1, 3]] = (boxes[:, [1, 3]] - top) / r
                last_boxes, last_scores = boxes, scores

            disp = frame.copy()
            for (x1, y1, x2, y2), sc in zip(last_boxes, last_scores):
                x1, y1 = max(int(x1), 0), max(int(y1), 0)
                x2, y2 = min(int(x2), disp.shape[1] - 1), min(int(y2), disp.shape[0] - 1)
                cv2.rectangle(disp, (x1, y1), (x2, y2), (0, 255, 0), 2)
                cv2.putText(disp, f"person {sc:.2f}", (x1, max(y1 - 6, 12)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

            fps_n += 1
            if time.time() - fps_t0 >= 1.0:
                fps = fps_n / (time.time() - fps_t0)
                fps_t0, fps_n = time.time(), 0
            state = "ON" if detect_on else "OFF"
            cv2.putText(disp, f"FPS {fps:.1f}  infer {infer_ms:.0f}ms  n={len(last_boxes)}  detect:{state}",
                        (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 220, 255), 1)

            if args.scale != 1.0:
                disp = cv2.resize(disp, None, fx=args.scale, fy=args.scale, interpolation=cv2.INTER_LINEAR)
            cv2.imshow(win, disp)

            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            if key == ord("s"):
                cv2.imwrite("snapshot_person.jpg", disp)
                print("已保存 snapshot_person.jpg")
            if key == ord("p"):
                detect_on = not detect_on
                print("检测", "开启" if detect_on else "关闭")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        cv2.destroyAllWindows()
        print("已退出")


if __name__ == "__main__":
    main()

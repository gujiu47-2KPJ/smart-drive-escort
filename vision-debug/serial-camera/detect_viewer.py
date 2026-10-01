#!/usr/bin/env python3
"""
智行护航 - 串口摄像头实时检测查看器（电脑端推理）

从 USB 串口(COM5)读取 ESP32-S3 推来的 JPEG 帧，在电脑上跑 nanoDet 做目标检测，
把检测框 + 类别 + 置信度画在画面上显示。

用法：
    python detect_viewer.py                       # 默认 COM5 @ 921600，全部类别
    python detect_viewer.py --only person         # 只显示行人
    python detect_viewer.py --conf 0.4 --nms 0.6  # 调阈值
    python detect_viewer.py --port COM9 --baud 115200

按键：q 退出 / s 保存当前帧 / p 暂停或继续检测
依赖：opencv-python、numpy、pyserial（本机已装）
"""

import argparse
import os
import sys
import time

import cv2
import numpy as np
import serial

# 让脚本能 import models/nanodet.py
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "models"))
from nanodet import NanoDet  # noqa: E402

FRAME_MAGIC0 = 0xAA
FRAME_MAGIC1 = 0xBB
MAX_FRAME_LEN = 200 * 1024

COCO_CLASSES = (
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag",
    "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon",
    "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot",
    "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
)


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


def letterbox(img, target_size=(416, 416)):
    """等比缩放 + 补边到 target_size，返回 (图, [top, left, newh, neww])。"""
    top, left, newh, neww = 0, 0, target_size[0], target_size[1]
    if img.shape[0] != img.shape[1]:
        hw_scale = img.shape[0] / img.shape[1]
        if hw_scale > 1:
            newh, neww = target_size[0], int(target_size[1] / hw_scale)
            img = cv2.resize(img, (neww, newh), interpolation=cv2.INTER_AREA)
            left = int((target_size[1] - neww) * 0.5)
            img = cv2.copyMakeBorder(img, 0, 0, left, target_size[1] - neww - left,
                                     cv2.BORDER_CONSTANT, value=0)
        else:
            newh, neww = int(target_size[0] * hw_scale), target_size[1]
            img = cv2.resize(img, (neww, newh), interpolation=cv2.INTER_AREA)
            top = int((target_size[0] - newh) * 0.5)
            img = cv2.copyMakeBorder(img, top, target_size[0] - newh - top, 0, 0,
                                     cv2.BORDER_CONSTANT, value=0)
    else:
        img = cv2.resize(img, target_size, interpolation=cv2.INTER_AREA)
    return img, [top, left, newh, neww]


def unletterbox(bbox, original_shape, letterbox_scale):
    """把 416 坐标系下的框映射回原始画面坐标。"""
    ret = bbox.astype(np.float64).copy()
    h, w = original_shape
    top, left, newh, neww = letterbox_scale
    if h == w:
        return (ret * (h / newh)).astype(np.int32)
    ratioh, ratiow = h / newh, w / neww
    ret[0] = max((ret[0] - left) * ratiow, 0)
    ret[1] = max((ret[1] - top) * ratioh, 0)
    ret[2] = min((ret[2] - left) * ratiow, w)
    ret[3] = min((ret[3] - top) * ratioh, h)
    return ret.astype(np.int32)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="ESP32-S3 串口摄像头 + 电脑端 nanoDet 检测")
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--model", default=os.path.join(here, "models", "nanodet.onnx"))
    ap.add_argument("--conf", type=float, default=0.35, help="置信度阈值")
    ap.add_argument("--nms", type=float, default=0.6, help="NMS IoU 阈值")
    ap.add_argument("--only", default="", help="只显示这些类别，逗号分隔，如 person")
    ap.add_argument("--scale", type=float, default=2.0, help="显示窗口放大倍数")
    args = ap.parse_args()

    keep = {c.strip() for c in args.only.split(",") if c.strip()} if args.only else None

    if not os.path.exists(args.model):
        print(f"找不到模型文件：{args.model}")
        sys.exit(1)
    print(f"加载模型 {args.model} ...")
    model = NanoDet(modelPath=args.model, prob_threshold=args.conf, iou_threshold=args.nms)
    print("模型就绪。")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:
        print(f"无法打开串口 {args.port}: {e}")
        sys.exit(1)

    print(f"正在监听 {args.port} @ {args.baud} ... 按 q 退出，s 保存，p 暂停检测")
    win = "ESP32-S3 detect"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)

    detect_on = True
    fps_t0, fps_n, fps = time.time(), 0, 0.0
    last_preds = np.array([])
    last_scale = None
    infer_ms = 0.0

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
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                lb, lb_scale = letterbox(rgb, (416, 416))
                t0 = time.time()
                preds = model.infer(lb)
                infer_ms = (time.time() - t0) * 1000.0
                last_preds, last_scale = preds, lb_scale

            display = frame.copy()
            if len(last_preds) > 0 and last_scale is not None:
                for pred in last_preds:
                    x1, y1, x2, y2 = unletterbox(pred[:4], display.shape[:2], last_scale)
                    conf = float(pred[-2])
                    cid = int(pred[-1])
                    if cid >= len(COCO_CLASSES):
                        continue
                    name = COCO_CLASSES[cid]
                    if keep is not None and name not in keep:
                        continue
                    cv2.rectangle(display, (x1, y1), (x2, y2), (0, 255, 0), 2)
                    label = f"{name} {conf:.2f}"
                    cv2.putText(display, label, (x1, max(y1 - 6, 12)),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

            fps_n += 1
            if time.time() - fps_t0 >= 1.0:
                fps = fps_n / (time.time() - fps_t0)
                fps_t0, fps_n = time.time(), 0
            state = "ON" if detect_on else "OFF"
            cv2.putText(display, f"FPS {fps:.1f}  infer {infer_ms:.0f}ms  detect:{state}",
                        (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 220, 255), 1)

            if args.scale != 1.0:
                display = cv2.resize(display, None, fx=args.scale, fy=args.scale,
                                     interpolation=cv2.INTER_LINEAR)
            cv2.imshow(win, display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            if key == ord("s"):
                cv2.imwrite("snapshot.jpg", display)
                print("已保存 snapshot.jpg")
            if key == ord("p"):
                detect_on = not detect_on
                print(f"检测已{'开启' if detect_on else '关闭'}")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        cv2.destroyAllWindows()
        print("已退出")


if __name__ == "__main__":
    main()

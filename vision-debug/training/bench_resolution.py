#!/usr/bin/env python3
"""在不同输入分辨率下验证同一个模型，量化分辨率对精度的影响。"""

import subprocess

YOLO = "/home/ebaina/venv-train/bin/yolo"
MODEL = "/home/ebaina/runs/person_widerperson/weights/best.pt"
DATA = "/home/ebaina/datasets/widerperson_yolo/data.yaml"


def main():
    print(f"{'imgsz':>6}  {'P':>7} {'R':>7} {'mAP50':>7} {'mAP50-95':>9}")
    for s in (640, 416, 320, 224):
        r = subprocess.run(
            [YOLO, "val", f"model={MODEL}", f"data={DATA}", f"imgsz={s}"],
            capture_output=True, text=True,
        )
        for line in r.stdout.splitlines():
            t = line.split()
            if len(t) >= 7 and t[0] == "all":
                # all  images  instances  P  R  mAP50  mAP50-95
                print(f"{s:>6}  {t[3]:>7} {t[4]:>7} {t[5]:>7} {t[6]:>9}")
                break
        else:
            print(f"{s:>6}  (val failed)")


if __name__ == "__main__":
    main()

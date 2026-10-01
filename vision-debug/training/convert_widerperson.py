#!/usr/bin/env python3
"""
把 HuggingFace 上的 finedet/widerperson（WiderPerson 户外行人数据集）
转换成 YOLO 单类 person 格式。

类别映射：pedestrian/rider/partially-visible-person -> person(0)
          crowd -> 丢弃（"一群人"的大框会污染检测训练）

输出：~/datasets/widerperson_yolo/{images,labels}/{train,val} + data.yaml
"""

import os

from datasets import load_dataset

OUT = os.path.expanduser("~/datasets/widerperson_yolo")
KEEP = {0, 1, 2}          # pedestrian, rider, partially-visible-person
CLASS_ID = 0              # 全部归为 person


def to_pil(img):
    """datasets 可能给 PIL.Image，也可能给 {bytes,path}，统一成 PIL。"""
    if hasattr(img, "size"):          # PIL.Image
        return img
    if isinstance(img, dict) and img.get("bytes"):
        import io
        from PIL import Image
        return Image.open(io.BytesIO(img["bytes"]))
    raise TypeError(f"unknown image type: {type(img)}")


def save_split(ds, split, verbose_schema=False):
    img_dir = os.path.join(OUT, "images", split)
    lbl_dir = os.path.join(OUT, "labels", split)
    os.makedirs(img_dir, exist_ok=True)
    os.makedirs(lbl_dir, exist_ok=True)

    if verbose_schema:
        print("columns:", ds.column_names)
        ex0 = ds[0]
        print("sample keys:", list(ex0.keys()))
        print("objects sample:", str(ex0.get("objects"))[:300])

    n_img = 0
    n_box = 0
    for i, ex in enumerate(ds):
        pil = to_pil(ex["image"])
        w, h = pil.size
        objs = ex["objects"]
        lines = []
        for bbox, cat in zip(objs["bbox"], objs["category"]):
            cat = int(cat)
            if cat not in KEEP:
                continue
            x, y, bw, bh = [float(v) for v in bbox]
            x1, y1 = max(0.0, x), max(0.0, y)
            x2, y2 = min(w, x + bw), min(h, y + bh)
            if x2 <= x1 or y2 <= y1:
                continue
            cx = (x1 + x2) / 2.0 / w
            cy = (y1 + y2) / 2.0 / h
            nw = (x2 - x1) / w
            nh = (y2 - y1) / h
            lines.append(f"{CLASS_ID} {cx:.6f} {cy:.6f} {nw:.6f} {nh:.6f}")
            n_box += 1

        name = f"{split}_{i:06d}"
        pil.convert("RGB").save(os.path.join(img_dir, name + ".jpg"), quality=92)
        with open(os.path.join(lbl_dir, name + ".txt"), "w") as f:
            f.write("\n".join(lines))
        n_img += 1

    print(f"[{split}] images={n_img} boxes={n_box}")


def main():
    print("loading finedet/widerperson ...")
    ds = load_dataset("finedet/widerperson")
    print("splits:", list(ds.keys()))
    save_split(ds["train"], "train", verbose_schema=True)
    save_split(ds["validation"], "val")

    yaml_path = os.path.join(OUT, "data.yaml")
    with open(yaml_path, "w") as f:
        f.write(
            f"path: {OUT}\n"
            "train: images/train\n"
            "val: images/val\n"
            "names:\n"
            "  0: person\n"
        )
    print("wrote", yaml_path)


if __name__ == "__main__":
    main()

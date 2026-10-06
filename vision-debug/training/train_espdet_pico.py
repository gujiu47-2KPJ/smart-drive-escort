#!/usr/bin/env python3
"""Train the bundled ESPDet-Pico model on the project's person dataset."""

import argparse
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-root",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Directory containing espdet_pico.yaml and the nn package",
    )
    parser.add_argument(
        "--esp-detection-root",
        type=Path,
        help="Legacy alias for an external ESP-Detection checkout",
    )
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--name", default="espdet_pico_224")
    parser.add_argument("--imgsz", type=int, default=224)
    parser.add_argument("--epochs", type=int, default=120)
    parser.add_argument("--batch", type=int, default=64)
    parser.add_argument("--device", default="0", help="CUDA device, or cpu")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--fraction", type=float, default=1.0)
    parser.add_argument("--patience", type=int, default=40)
    args = parser.parse_args()

    root = (args.esp_detection_root or args.model_root).resolve()
    if not (root / "nn" / "esp_tasks.py").exists():
        raise FileNotFoundError(f"ESPDet model root not found: {root}")
    sys.path.insert(0, str(root))

    import ultralytics.nn.tasks as tasks
    from nn.esp_tasks import custom_parse_model
    from ultralytics import YOLO

    tasks.parse_model = custom_parse_model
    model_yaml = root / "espdet_pico.yaml"
    if not model_yaml.exists():
        model_yaml = root / "cfg" / "models" / "espdet_pico.yaml"
    if not model_yaml.exists():
        raise FileNotFoundError(f"ESPDet YAML not found below {root}")
    model = YOLO(str(model_yaml))
    params = sum(p.numel() for p in model.model.parameters())
    print(f"ESPDet-Pico parameters: {params:,}")

    train_args = dict(
        data=str(args.data.resolve()),
        project=str(args.project.resolve()),
        name=args.name,
        imgsz=args.imgsz,
        epochs=args.epochs,
        batch=args.batch,
        device=args.device,
        workers=args.workers,
        fraction=args.fraction,
        patience=args.patience,
        close_mosaic=min(30, args.epochs // 3),
        optimizer="auto",
        mosaic=1.0,
        mixup=0.0,
        copy_paste=0.1,
        pretrained=False,
        exist_ok=True,
    )
    print("Training settings:", train_args)
    results = model.train(**train_args)
    best = Path(results.save_dir) / "weights" / "best.pt"
    print(f"best checkpoint: {best}")


if __name__ == "__main__":
    main()

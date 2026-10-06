#!/usr/bin/env python3
"""Reproduce the ESPDet-Pico parameter count from the bundled YAML and nn."""

import argparse
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-root",
        type=Path,
        default=Path(__file__).resolve().parent,
    )
    args = parser.parse_args()

    root = args.model_root.resolve()
    sys.path.insert(0, str(root))
    import ultralytics.nn.tasks as tasks
    from nn.esp_tasks import custom_parse_model
    from ultralytics import YOLO

    tasks.parse_model = custom_parse_model
    model = YOLO(str(root / "espdet_pico.yaml"))
    total = sum(p.numel() for p in model.model.parameters())
    trainable = sum(p.numel() for p in model.model.parameters() if p.requires_grad)
    print(f"total_parameters={total}")
    print(f"trainable_parameters={trainable}")
    print(f"float32_parameter_bytes={total * 4}")


if __name__ == "__main__":
    main()

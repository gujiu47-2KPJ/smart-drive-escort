#!/usr/bin/env python3
"""Post-training INT8 quantization for an ESPDet-Pico ONNX model."""

import argparse
import os
from pathlib import Path

import onnx
import torch
from PIL import Image
from onnxsim import simplify
from torch.utils.data import DataLoader, Dataset
from torchvision import transforms

from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx


class CalibrationImages(Dataset):
    def __init__(self, directory: Path, size: int):
        self.paths = sorted(
            p for p in directory.iterdir() if p.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp"}
        )
        if not self.paths:
            raise FileNotFoundError(f"no calibration images in {directory}")
        self.transform = transforms.Compose([transforms.ToTensor(), transforms.Resize((size, size))])

    def __len__(self):
        return len(self.paths)

    def __getitem__(self, index):
        with Image.open(self.paths[index]) as image:
            return self.transform(image.convert("RGB"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--onnx", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--calib-dir", required=True, type=Path)
    parser.add_argument("--imgsz", type=int, default=224)
    parser.add_argument("--target", default="esp32s3", choices=("esp32s3", "esp32p4"))
    parser.add_argument("--calib-steps", type=int, default=32)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--error-report", action="store_true", help="Run the very slow layerwise PPQ error analysis")
    args = parser.parse_args()

    model = onnx.load(str(args.onnx))
    model, check = simplify(model)
    if not check:
        raise RuntimeError("ONNX simplification failed")
    onnx.save(onnx.shape_inference.infer_shapes(model), str(args.onnx))

    dataset = CalibrationImages(args.calib_dir.resolve(), args.imgsz)
    loader = DataLoader(dataset, batch_size=args.batch, shuffle=False, num_workers=0)

    def collate_fn(batch):
        if isinstance(batch, torch.Tensor):
            return batch.to("cpu")
        return torch.stack(tuple(batch)).to("cpu")

    setting = QuantizationSettingFactory.espdl_setting()
    setting.equalization = True
    setting.equalization_setting.iterations = 4
    setting.equalization_setting.value_threshold = 0.4
    setting.equalization_setting.opt_level = 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    espdl_quantize_onnx(
        onnx_import_file=str(args.onnx),
        espdl_export_file=str(args.output),
        calib_dataloader=loader,
        calib_steps=min(args.calib_steps, len(loader)),
        input_shape=[1, 3, args.imgsz, args.imgsz],
        target=args.target,
        num_of_bits=8,
        collate_fn=collate_fn,
        setting=setting,
        device="cpu",
        error_report=args.error_report,
        skip_export=False,
        export_test_values=False,
        verbose=0,
    )
    print(f"quantized: {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()

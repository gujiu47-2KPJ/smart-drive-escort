# ESPDet-Pico 行人模型

本目录记录 ESPDet-Pico 在 WiderPerson 单类 `person` 数据集上的训练与 ESP32-S3 部署产物。

模型定义已随仓库交付：`espdet_pico.yaml` 和 `nn/`。`best.pt` 依赖其中的自定义 `DSConv`、`ESPBlock*`、`DSC3k2`、`ESPDetect` 以及 `custom_parse_model`，不能只拿权重文件而不带这些源码。

## 结果

| 项目 | 结果 |
|---|---:|
| 输入 | 224 x 224 |
| 参数量 | 361,167 |
| FP32 mAP50 | 0.51738 |
| FP32 mAP50-95 | 0.28139 |
| INT8 `.espdl` | 492,032 bytes |
| 目标 | ESP32-S3 |

训练集为 8,000 张图、232,830 个框，验证集为 1,000 张图、27,353 个框。FP32 指标来自完整验证集；INT8 模型已通过 ESP-PPQ 导出检查，但量化后的 mAP 尚未在当前脚本中自动复算。

## 复现

```bash
source ~/venvs/escort-train/bin/activate

python vision-debug/training/convert_widerperson.py \
  --source-dir /mnt/d/WSL/training-data/widerperson-hf/data \
  --output-dir ~/datasets/widerperson_yolo

python vision-debug/training/train_espdet_pico.py \
  --data ~/datasets/widerperson_yolo/data.yaml \
  --project ~/outputs/espdet_pico_widerperson \
  --name espdet_pico_224 --imgsz 224 --epochs 120 --batch 64 --device 0

python vision-debug/training/count_espdet_params.py
```

参数量统计就是模型构建完成后对所有参数张量求元素个数：

```python
sum(p.numel() for p in model.model.parameters())
```

该命令在当前 YAML 和 `nn` 源码下输出 `361167`；它统计的是可训练网络参数，不是 `.pt` 文件大小，也不是量化后 `.espdl` 文件大小。

导出和量化：

```bash
python vision-debug/training/export_espdet_pico.py \
  --esp-detection-root /mnt/d/WSL/training-data/esp-detection-main \
  --weights ~/outputs/espdet_pico_widerperson/espdet_pico_224/weights/best.pt \
  --imgsz 224

python vision-debug/training/quantize_espdet_pico.py \
  --onnx ~/outputs/espdet_pico_widerperson/espdet_pico_224/weights/best.onnx \
  --output ~/outputs/espdet_pico_widerperson/espdet_pico_224/weights/best.espdl \
  --calib-dir ~/datasets/widerperson_yolo/images/val \
  --imgsz 224 --target esp32s3 --calib-steps 32 --batch 32
```

`best.espdl` 应复制到 ESP-DL 的 ESPDet 模型组件中，并使用对应的 ESPDet postprocessor。真实板上帧率必须在 ESP32-S3 + OV2640 上烧录后记录；本机 GPU 推理速度不能替代板上实测。

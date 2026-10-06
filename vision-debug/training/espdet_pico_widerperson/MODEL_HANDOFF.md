# ESPDet-Pico 行人模型交接

## 模型文件

| 文件 | 用途 | 大小 |
|---|---|---:|
| `best.pt` | PyTorch 训练权重 | 1,005,909 bytes |
| `best.onnx` | 固定输入 ONNX，供检查/量化 | 1,461,635 bytes |
| `best.espdl` | ESP-DL / ESP32-S3 INT8 模型 | 492,032 bytes |
| `espdet_pico.yaml` | 模型结构配置 | 1,563 bytes |

模型源码：`../nn/`，模型结构：`../espdet_pico.yaml`。其中 `esp_tasks.py` 注册 YAML 解析器，`modules/esp_conv.py`、`esp_block.py`、`esp_head.py` 实现 ESPDet 自定义层；加载 `best.pt` 时必须保留该目录并安装匹配的 Ultralytics/PyTorch 依赖。

SHA-256：

```text
best.pt    7f0902ec2dfb98b8d965212975ac1e4475ce8d1e68fd9f7ad1a592e37b110bdc
best.onnx  d95ba74a5c3d494c329932febf54effb8c864676609b66bd2ff5f47302ad4b83
best.espdl 8391b99ea1d32ea3b9dedfa0e239b6d9cd9c317164c2cf83bf6ffb0d774f9f1c
```

## 验证结果

- 数据集：WiderPerson，单类 `person`
- 训练集：8,000 张，232,830 个框
- 验证集：1,000 张，27,353 个框
- 输入尺寸：224 x 224
- 参数量：361,167
- 最佳 epoch：120
- Precision：0.70536
- Recall：0.45560
- mAP50：0.51738
- mAP50-95：0.28139

FP32 指标来自完整验证集。当前已完成 ESP-PPQ INT8 导出，`.espdl` 大小为 492 KB；量化后 mAP 和真实 ESP32-S3 + OV2640 帧率仍需单独实测。

参数量复现：

```bash
python vision-debug/training/count_espdet_params.py
```

统计公式为 `sum(p.numel() for p in model.model.parameters())`，输出总参数量 `361167`；参数量与权重文件大小不是同一个指标。

## 验证证据

- `results.csv`：每个 epoch 的训练/验证指标
- `results.png`：损失、Precision、Recall、mAP 曲线
- `confusion_matrix.png`：验证集混淆矩阵
- `BoxPR_curve.png`：Precision-Recall 曲线
- `BoxP_curve.png`：Precision 曲线
- `BoxR_curve.png`：Recall 曲线
- `val_batch*_labels.jpg` / `val_batch*_pred.jpg`：验证集标注与预测对照

## 部署注意

ESP32-S3 端应使用 ESP-DL 的 ESPDet 后处理，输入为 RGB 224 x 224，并使用模型导出的 3 个检测尺度。`best.espdl` 不能直接用普通 ONNX Runtime 加载；板端加载和帧率验证需要 ESP-IDF + ESP-DL 工程。

# ESPDet-Pico FP32 and INT8 comparison

This record covers the WiderPerson single-class `person` model exported for ESP32-S3.

| Artifact | Format | Size | Validation status |
| --- | --- | ---: | --- |
| `best.pt` | PyTorch FP32 checkpoint | 1,005,909 bytes | Validated on the full validation split |
| `best.onnx` | ONNX FP32 export | 1,461,635 bytes | ONNX checker passed |
| `best.espdl` | ESP-DL INT8 export | 492,032 bytes | 216 operators quantized; export completed |

## FP32 validation

The FP32 checkpoint was evaluated on 1,000 validation images and 27,353 boxes at
224 x 224 input resolution:

| Metric | Value |
| --- | ---: |
| Precision | 0.70536 |
| Recall | 0.45560 |
| mAP50 | 0.51738 |
| mAP50-95 | 0.28139 |

The training run used 8,000 images and 232,830 boxes. The best checkpoint was
recorded at epoch 120; the complete per-epoch record is in `results.csv`.

## INT8 status

The INT8 artifact was produced with ESP-PPQ using 32 calibration steps from the
WiderPerson training images, targeting `esp32s3`. The current export pipeline
checks the quantized graph and file generation, but does not yet run the INT8
model through the Ultralytics validation loop. Therefore an INT8 mAP value is
not claimed here. A final deployment report must add:

1. INT8 mAP on the same validation split using the ESP-DL/ESPDet postprocessor.
2. Measured FPS, latency, and peak memory on the target ESP32-S3 + OV2640 board.

The `.espdl` file is not an ONNX Runtime model; use the matching ESP-DL ESPDet
component and postprocessor on the board.

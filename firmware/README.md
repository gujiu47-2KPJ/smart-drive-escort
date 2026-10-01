# 智行护航 firmware

ESP32-S3-N16R8 / ESP-IDF v6.0.2 固件工程。当前是 Phase 1 + Phase 2 骨架。

## 环境

| 项 | 值 |
|---|---|
| ESP-IDF | v6.0.2（`D:\esp\v6.0.2\esp-idf`） |
| 工具链 | `D:\Espressif` |
| 编辑器 | VSCode + espressif.esp-idf-extension 2.3.0 |
| 目标芯片 | esp32s3（16MB Flash + 8MB Octal PSRAM） |

用 VSCode 打开本目录（`G:\codex-workspace\firmware`），
按 `F1` → `ESP-IDF: Build your project`。

## 目录结构

```
firmware/
├── CMakeLists.txt          工程入口
├── sdkconfig.defaults      默认配置（PSRAM / 分区表 / FreeRTOS 时基）
├── partitions.csv          分区表（factory 4MB + storage 2MB）
└── main/
    ├── CMakeLists.txt
    ├── board_config.h      板型与引脚，全部引脚只在这里定义
    └── main.c              Phase 1 + Phase 2 演示
```

## 当前实现

| 任务 | 职责 | 优先级 | 栈（字） | 通信 |
|---|---|---|---|---|
| `radar_sim` | 模拟雷达周期采集 | 5 | 3072 | → `s_sensor_queue` |
| `fusion` | 融合 + 风险判定 + 统计 | 4 | 4096 | 收队列；Mutex 保护统计；`xQueueOverwrite` → `s_led_queue` |
| `button` | 响应 BOOT 按键事件 | 3 | 3072 | 等 `s_button_sem`（ISR 给出） |
| `led` | 按风险等级驱动板载 LED | 2 | 2048 | 收 `s_led_queue` |

预期现象：串口 115200 打印芯片信息与融合日志；板载 LED 随"目标距离"变化，
安全慢闪（1s）、警告中闪（300ms）、危险快闪（100ms）。

## 引脚（Freenove ESP32-S3 WROOM）

板载：LED = GPIO2，WS2812 = GPIO48，BOOT 键 = GPIO0

摄像头（OV2640 DVP）：XCLK 15 / PCLK 13 / VSYNC 6 / HREF 7 / SIOD 4 / SIOC 5
/ D0-D7 = 11, 9, 8, 10, 12, 18, 17, 16

microSD（SDMMC 1-bit）：CLK 39 / CMD 38 / D0 40

传感器预留（尚未接线）：

| 模块 | 总线 | 引脚 |
|---|---|---|
| 雷达 MS60-1211S80M | UART1 @ 921600 | TX=GPIO14，RX=GPIO21 |
| IMU HW-991 (BMI270) | I2C0 | SDA=GPIO1，SCL=GPIO47 |

已被占用、不可再用的引脚：16-18（摄像头）、35/36/37（Octal PSRAM，数据手册明确）、
38/39/40（SD）、4-13/15（摄像头）、19/20（USB）、43/44（串口日志）、48（WS2812）。
GPIO0/3/45/46 是 strapping 脚，尽量别外接。

## 下一步

1. 确认主控板是"相机板"还是"最小板"，以及"单板还是双板"架构。
2. Phase 3-A：接入雷达（UART，解析 0x5A 主动上报帧）。
3. Phase 3-B：接入 IMU（I2C，BMI270）。

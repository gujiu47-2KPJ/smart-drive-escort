# 调试日志（项目传感器相关）

记录本项目的关键调试结论，避免重蹈覆辙。环境/MCP 的坑另见
`Technical Support Document/Codex环境与MCP排错手册.md`。

## 雷达（MS60-1211S80M / AT6010）

- **协议是逆向出来的**：官方上位机 `ATRadarSettingTool` 的 `airhost.dll` 反汇编出命令表。帧格式与 HCI 协议文档一致。
- **`0xFE`（读版本）命令不存在**——最早那版固件发 0xFE 无应答，不是接线问题，是模块根本不支持这条命令。
- **模块是从机**：不主动发数据，被动监听一条安静的线收到 0 字节是预期。
- **OUT 引脚不含数据**：只是存在信号（1-bit），反复读 OUT 是白费。
- **精度归因结论**：瓶颈在模块内部算法 + 物理 + 参数，不在我们的协议解析（逐字节已核对）。用 `rb_conf`/`angle_conf` 过滤低置信度帧是零成本改进。

## 摄像头（OV2640，ESP-IDF esp_video）

- **现象**：每帧只收到 1/8 数据，日志 `E:RX:115200-14400`；换大分辨率格式又报 I2C 超时。
- **根因与修复（两个坑）**：
  1. RGB565（非 JPEG）模式下 DVP 驱动对帧大小做严格匹配 → 换 **JPEG 格式**绕开。
  2. SCCB 400kHz 写寄存器表挂总线 → 降到 **100kHz**（Arduino esp32-camera 用的速率）。
- **XCLK**：BSP 硬编码 16MHz，但 OV2640 驱动格式表全写 `20Minput` → 改成 **20MHz**。
- **硬件验证**：用 Arduino CameraWebServer 确认摄像头硬件本身是好的，问题全在 esp_video 驱动配置。
- **端口**：板子串口是 **COM5（CH343）**；COM9 是另一块 CH340 USB-TTL，别烧错。

## 选型结论（雷达）

- **速度是第一位**，现有模块无速度 → 速度交给摄像头+IMU。
- BSD 版（TYPE 7）看似有速度+8目标，但：测速下限 6km/h（≈农机最高速）、建立时间 5s、距离 s8/米粒度 → **排除**。
- 低价位候选：HLK-LD2450（24G、3目标、有速度，但 5V + 1T2R）；LD6004（60G 2T2R 3.3V 排针封装，测距精度 0.4m）。
- 详细对比见 `Technical Support Document/雷达选型与精度分析.md`。

## 行人检测（ESP-WHO，已跑通）

- 工程：`vision-debug/esp-who/examples/object_detect`，模型 `pedestrian_detect`。
- 已把摄像头改成 JPEG + 解码节点，检测能出框（含 Web 推流，见 toolchain.md）。

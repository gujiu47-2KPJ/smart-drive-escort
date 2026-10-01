# 工具链与构建/烧录

## ESP-IDF

- 版本 **v6.0.2**：`D:\esp\v6.0.2\esp-idf`；工具链 `D:\Espressif`；Python 环境 `D:\Espressif\python_env\idf6.0_py3.12_env`。
- 构建/烧录辅助脚本：`G:\codex-workspace\vision-debug\idf-pedestrian.ps1`（行人检测工程用）。
  ```
  cd G:\codex-workspace\vision-debug
  .\idf-pedestrian.ps1 build
  .\idf-pedestrian.ps1 -Port COM5 flash
  ```

## 两个 ESP-IDF 特有的坑

1. **PowerShell 会把 `-DSDKCONFIG_DEFAULTS=xxx` 拆散**，导致 idf.py 报 `No such option: -D`。
   解决：改环境变量传参。ESP-WHO 的扩展 `tools/bsp_ext.py` 支持从环境变量读
   `SDKCONFIG_DEFAULTS` 和 `DETECT_MODEL`。
2. **这两个变量必须对每一条 idf.py 命令都设**（不只 `set-target`），否则 `idf.py build` 报
   `SDKCONFIG_DEFAULTS is not set`。

## 串口

- 板子：**COM5**（CH343）。
- COM9 是另一块 CH340 USB-TTL 转接板，不是开发板。
- 波特率 115200。

## 快速硬件验证（Arduino，仅诊断用）

- 用 PlatformIO（`D:\platformio\penv\Scripts\platformio.exe`）+ `freenove_esp32_s3_wroom` 板。
- CameraWebServer 诊断工程：`G:\codex-workspace\vision-debug\camera-test\`。
- 注意：项目主体必须用 ESP-IDF（AGENTS.md 规则），Arduino 只用于排查硬件。

## Rust（嵌入式调试 MCP 依赖，已装）

- `RUSTUP_HOME=D:\Rust\rustup`、`CARGO_HOME=D:\Rust\cargo`（不在 C 盘）。
- MinGW GNU 工具链（本机无 MSVC），链接器在 `E:\c\MinGW\bin`。

# Codex 环境与 MCP 排错手册

> 首次生成：2026-10-02
> 适用机器：TIANXUAN\gujiu（Windows）
> 目的：环境冲突或 MCP 失效时，能在这里快速定位原因并恢复

本文记录本机 Codex 的 MCP 服务器、插件、技能配置，以及**踩过的坑和对应处置**。
凡是"看起来不行、其实没问题"的情况都单独标注，避免重复排查。

---

## 一、总览

### 1.1 MCP 服务器

配置文件：`C:\Users\gujiu\.codex\config.toml`

| 名称 | 用途 | 启动命令 | 状态 |
|---|---|---|---|
| `serial` | 串口读写（雷达/IMU 抓包） | `D:\opencode\venv-mcp1\Scripts\python.exe -m serial_mcp_server.server` | 启用 |
| `embedded_debugger` | 片上调试（断点/内存/RTT/崩溃诊断） | `D:\Rust\bin\embedded-debugger-mcp.exe serve` | 启用 |
| `cua_repl` | Codex 自带 | `ChatGPT.exe` | **禁用**（enabled=false） |

### 1.2 插件（11 个已启用）

| 插件 | 来源市场 |
|---|---|
| documents / pdf / spreadsheets / presentations / template-creator | openai-primary-runtime |
| codex-app-tools / visualize | openai-bundled |
| **embedded-workbench** | embedded-workbench-dev（Git） |
| **github / codex-security / superpowers** | openai-api-curated |

额外市场：`embedded-workbench-dev` → `https://github.com/AmethystLuna/embedded-workbench.git`

### 1.3 技能（`C:\Users\gujiu\.codex\skills\`）

| 技能 | 用途 |
|---|---|
| `embedded-debugger` | 配合 embedded_debugger MCP 的调试流程 |
| `embedded-engineering` | 硬件/协议/RF 设计 |
| `esp32-dev` | ESP-IDF 分层架构与规范 |

（插件自带的技能如 `embedded-firmware-dev`、`debug-methodology` 等，
位于 `C:\Users\gujiu\.codex\plugins\cache\`，不在上面的目录里。）

### 1.4 关键环境变量（用户级，持久化）

| 变量 | 值 | 作用 |
|---|---|---|
| `RUSTUP_HOME` | `D:\Rust\rustup` | Rust 工具链位置（**不在 C 盘**） |
| `CARGO_HOME` | `D:\Rust\cargo` | Cargo 缓存与 bin 位置 |

用户 PATH 末尾追加了 `D:\Rust\cargo\bin`（追加，不是替换）。

---

## 二、MCP 详情与健康检查

### 2.1 serial（串口 MCP）

| 项 | 值 |
|---|---|
| 来源 | 从本机 ReasonIX / OpenCode 配置中提取 |
| 运行时 | Python 3.12.10 @ `D:\opencode\venv-mcp1` |
| 依赖 | `mcp 1.29.0`、`pyserial 3.5` |
| 服务器标识 | `serial-mcp-server v1.29.0` |
| 工具数 | **27** |

**健康检查**（不需要板子）：

```powershell
D:\opencode\venv-mcp1\Scripts\python.exe G:\codex-workspace\tools\test-serial-mcp.py
```

期望输出：`server name : serial-mcp-server` + `tool count: 27` + `RESULT: PASS`

**为什么用独立 venv**：`serial_mcp_server` 用的是 **mcp 1.x** API，
而系统 Python 里的 `cubemx_mcp` 需要 **mcp 2.x**，两者冲突。
所以这个 MCP 必须用 `D:\opencode\venv-mcp1` 里的解释器，不能换成系统 Python。

**常见故障**：

| 现象 | 原因 | 处置 |
|---|---|---|
| 启动即失败 | 用了系统 Python | 改回 `D:\opencode\venv-mcp1\Scripts\python.exe` |
| 读不到串口数据 | 串口被其他程序占用 | 关掉串口助手 / 厂商上位机 |
| 端口列表为空 | 驱动未装 | CH34x 驱动（CAM 板资料里有） |

### 2.2 embedded_debugger（片上调试 MCP）

| 项 | 值 |
|---|---|
| 来源 | `https://github.com/Adancurusul/embedded-debugger-mcp` (MIT) |
| 服务器标识 | `rmcp 0.3.2`；程序版本 `0.3.0` |
| 二进制 | `D:\Rust\bin\embedded-debugger-mcp.exe` |
| 源码 | `G:\codex-workspace\tools\embedded-debugger-mcp` |
| 编译环境 | Rust 1.99.0 / `x86_64-pc-windows-gnu`（MinGW） |
| 工具数 | **24** |

**工具清单**：`list_probes` `probe_info` `connect` `disconnect` `get_status`
`halt` `run` `step` `reset` `set_breakpoint` `clear_breakpoint`
`read_memory` `write_memory` `flash_program` `flash_erase` `flash_verify`
`run_firmware` `rtt_attach` `rtt_detach` `rtt_read` `rtt_write` `rtt_channels`
`diagnose_fault` `unwind_exception`

**健康检查**：

```powershell
# 1) 环境自检
D:\Rust\bin\embedded-debugger-mcp.exe doctor
#    期望: config_valid: true

# 2) MCP 握手测试
C:\Users\gujiu\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe `
  G:\codex-workspace\tools\test-debugger-mcp.py
#    期望: tool count: 24 + RESULT: PASS
```

**调试后端**：`connect` 时二选一
- 默认 **probe-rs**（原生，支持 RTT）
- 指定 `backend="openocd"` 走 GDB RSP

> ⚠ **OpenOCD 路径注意**：走 openocd 后端时用的是 PATH 里的 openocd。
> 当前 PATH 中是 **`E:\tools\openocd\xpack-openocd-0.12.0-7`（通用版）**。
> 调 ESP32-S3 若识别不到芯片，改用 ESP-IDF 自带的：
> `D:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\bin\openocd.exe`

> 💡 ESP32-S3 有**内置 USB-JTAG**，理论上不需要外接调试器，
> 但要求板子把原生 USB 口（GPIO19/20）引出来。

**重新编译**（改过源码或二进制丢失时）：

```powershell
$env:RUSTUP_HOME='D:\Rust\rustup'; $env:CARGO_HOME='D:\Rust\cargo'
$env:PATH="D:\Rust\cargo\bin;E:\c\MinGW\bin;$env:PATH"
D:\Rust\cargo\bin\cargo.exe build --release `
  --manifest-path G:\codex-workspace\tools\embedded-debugger-mcp\Cargo.toml
Copy-Item G:\codex-workspace\tools\embedded-debugger-mcp\target\release\embedded-debugger-mcp.exe `
  D:\Rust\bin\ -Force
```

### 2.3 cua_repl

Codex 自带，`enabled = false`。**不要动它。** 如果它变成启用状态，
说明 config.toml 被别的工具改写过，检查一下其他改动。

---

## 三、已知陷阱（重点，按踩坑时间排序）

### 陷阱 1：Codex 沙箱会"虚拟化"注册表 ⭐ 2026-10-02 发现

**现象**：在沙箱内（普通命令）读环境变量，`RUSTUP_HOME` / `CARGO_HOME` 显示为空，
`HKCU:\Environment\Path` 显示成只有 51 字符的残缺值——
看起来像"环境变量没写进去"或"PATH 被覆盖了"。

**真相**：**全是沙箱给的虚拟化视图，注册表里的真实值是正确的。**

**验证方法**：同样的读取命令加上提升权限（沙箱外）执行：

```powershell
# 沙箱内（会显示假的空值）
[Environment]::GetEnvironmentVariable('RUSTUP_HOME','User')

# 沙箱外（显示真实值）——需要提升权限运行
(Get-ItemProperty HKCU:\Environment -Name RUSTUP_HOME).RUSTUP_HOME
```

**结论**：**排查环境变量问题时，必须用非沙箱方式读取。**
在沙箱里读到的空值/残缺值不可信。

### 陷阱 2：环境变量在进程创建时固定 ⭐ 2026-10-02 发现

**现象**：注册表里 `RUSTUP_HOME` 明明是对的，但在当前会话里跑 `cargo --version`
却报 `rustup could not choose a version of cargo to run`，
并且会在 `C:\Users\gujiu\.rustup` 建一个空目录。

**原因**：Codex 应用进程是 **13:58 启动**的，而环境变量是 **16:48 写入**的。
**已启动的进程不会感知新写入的环境变量**，于是 rustup 退回默认位置
`C:\Users\gujiu\.rustup`，那里没有工具链，就报错。

**处置**：
1. **重启 Codex**（或新开一个从资源管理器启动的终端）
2. 清掉误建的空目录 `C:\Users\gujiu\.rustup`（内容为空，可直接删）

**判断方法**：

```powershell
Write-Host $env:RUSTUP_HOME   # 有值 = 当前进程看得见；空 = 需要重启
```

### 陷阱 3：Codex 沙箱的 ACL 会让 .git 卡死整个沙箱 ⭐ 2026-10-02 已修复

**现象**：所有普通（非提升权限）命令失败：
`helper_unknown_error: setup refresh had errors`，只有提升权限能跑。

**原因**：Codex 沙箱要在可写工作区内给 `.git` 写一条 deny ACE 来保护它，
这需要 `WRITE_DAC` 权限。而 `G:\codex-workspace\.git` 的所有者是
**沙箱账户 `TIANXUAN\CodexSandboxOffline`**
（因为 `.git` 是 10-01 在沙箱内跑 `git init` 创建的），
当前身份既不是所有者、`Modify` 又不含 `WRITE_DAC`，于是写入被拒，沙箱初始化中止。

**证据**：`C:\Users\gujiu\.codex\.sandbox\sandbox.<日期>.log`

```
deny ACE failed on G:\codex-workspace\.git: open deny ACL target for update
setup refresh completed with errors
```

时间线完全吻合：`sandbox.2026-06-06.log` 和 `09-30.log` 无此错误，
`10-01.log` 第 950 行首次出现（正是 `.git` 创建之后）。

**修复**：把 `.git` 所有者改回真实用户。
脚本：`G:\codex-workspace\tools\fix-sandbox-git-acl.ps1`（自带提权，ACL 先备份）

**复发条件**：如果将来又在沙箱内 `git init`，`.git` 会再次变成沙箱账户所有。
届时重跑一次修复脚本即可。

### 陷阱 4：PowerShell 会拆散 `-D` 参数

**现象**：`idf.py -DSDKCONFIG_DEFAULTS=xxx` 报 `No such option: -D`。

**原因**：PowerShell 把 `-D...` 参数拆成了两截传给 idf.py。

**处置**：改用**环境变量**传参。ESP-WHO 的 `tools/bsp_ext.py` 本来就支持
从环境变量读取 `SDKCONFIG_DEFAULTS` 和 `DETECT_MODEL`。

### 陷阱 5：ESP-WHO 的两个变量必须"每条命令都设"

**现象**：`set-target` 成功，紧接着 `idf.py build` 报
`SDKCONFIG_DEFAULTS is not set`。

**原因**：`bsp_ext.py` 的全局回调对 `set-target` / `reconfigure` / `all`
三种任务都做同样的检查，**不只是 `set-target`**。

**处置**：见 `G:\codex-workspace\vision-debug\idf-pedestrian.ps1`，
它每次都把两个变量设好再调 `idf.py`。

### 陷阱 6：用户 PATH 里有失效条目

机器级 PATH 中存在**目录不存在**的条目（软件已卸载但 PATH 没清）：

```
C:\ST\STM32CubeCLT_1.18.0 及其 5 个子目录
C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Ut   ← 路径被截断
C:\Program Files\AskLink
D:\MSYS2\mingw64\bin
F:\yolov8 trainng\Git\cmd
D:\opencode\app
```

其中 `STM32 ST-LINK Ut` 是**被截断的完整路径**，说明 PATH 曾被手工编辑或写入异常。

**处置脚本**（默认 dry-run，不写任何东西）：
`G:\codex-workspace\tools\repair-path-and-profile.ps1`

### 陷阱 7：PowerShell 5.1 与 7 的编码差异

| 配置文件 | 编码 | 结果 |
|---|---|---|
| `Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1`（5.1） | UTF-8 **带 BOM** | 正常 |
| `Documents\PowerShell\Microsoft.PowerShell_profile.ps1`（7） | UTF-8 **无 BOM** | 中文变乱码 |

**后果**：含中文的 `.ps1` 脚本在 Windows PowerShell 5.1 下按 GBK 解码，
可能报"缺少右括号"之类的诡异语法错误。

**规避**：**给 AI/脚本用的 .ps1 一律写成纯 ASCII**，或确保带 UTF-8 BOM。

---

## 四、故障速查表

| 症状 | 最可能的原因 | 先做这个 |
|---|---|---|
| 所有普通命令失败，只有提升权限能跑 | 陷阱 3（沙箱 ACL） | 看 `.sandbox\sandbox.<日期>.log` |
| 环境变量读出来是空的 | 陷阱 1（沙箱虚拟化） | 用非沙箱方式重读 |
| 刚设的变量在当前会话里看不到 | 陷阱 2（进程启动时固定） | 重启 Codex |
| `cargo` 报没有默认工具链 | 陷阱 2 + 缺 `RUSTUP_HOME` | 重启；确认变量已在注册表 |
| MCP 工具在会话里不存在 | 应用未重启 | 重启；MCP 是启动时加载的 |
| `idf.py` 报 `No such option: -D` | 陷阱 4 | 改用环境变量 |
| `idf.py build` 报 `SDKCONFIG_DEFAULTS is not set` | 陷阱 5 | 用 `idf-pedestrian.ps1` |
| 中文脚本报奇怪的语法错误 | 陷阱 7（编码） | 脚本改纯 ASCII |
| 串口 MCP 读不到数据 | 端口被占用 | 关掉上位机/串口助手 |
| 调试器找不到探针 | 板子没插 / 驱动 | 插板子；`embedded-debugger-mcp doctor` |

---

## 五、回滚点

所有备份在 `G:\codex-workspace\backup\`：

| 目录 | 内容 |
|---|---|
| `codex-config-20261002-141426\config.toml.bak` | 加 serial MCP 前的 config.toml |
| `codex-config-20261002-165003\config.toml.bak` | 加 embedded_debugger 前的 config.toml |
| `env-rust\PATH.before.txt` | 改用户 PATH 前的原值 |
| `env-rust\RUSTUP_HOME.before.txt` | 原值（空） |
| `env-rust\CARGO_HOME.before.txt` | 原值（空） |
| `git-acl-20261002-142120\git-acl.txt` | `.git` 的 ACL 快照 |
| `20260930-env-fix\` + `restore-env.ps1` | 更早的一次环境修复备份 |

**恢复 config.toml**：

```powershell
Copy-Item 'G:\codex-workspace\backup\codex-config-20261002-165003\config.toml.bak' `
          'C:\Users\gujiu\.codex\config.toml' -Force
```

**恢复 `.git` ACL**：

```powershell
icacls G:\codex-workspace /restore "G:\codex-workspace\backup\git-acl-20261002-142120\git-acl.txt"
```

---

## 六、变更记录

| 日期 | 变更 | 说明 |
|---|---|---|
| 2026-10-01 | 工作区 git 初始化 | 后续触发了陷阱 3 |
| 2026-10-02 14:14 | 加入 `serial` MCP | 握手验证 27 工具 |
| 2026-10-02 14:21 | 修复沙箱 ACL（`.git` 所有者） | 陷阱 3 根治 |
| 2026-10-02 16:34 | 安装 `embedded-workbench` 插件 | 4 代理 + 8 技能 |
| 2026-10-02 16:39 | 安装 `esp32-dev`、`embedded-engineering` 技能 | |
| 2026-10-02 16:42 | 安装 `github`、`codex-security`、`superpowers` 插件 | |
| 2026-10-02 16:43 | 安装 Rust 到 `D:\Rust`（MinGW GNU 工具链） | **不占 C 盘** |
| 2026-10-02 16:50 | 加入 `embedded_debugger` MCP | 握手验证 24 工具 |
| 2026-10-02 16:52 | 持久化 `RUSTUP_HOME` / `CARGO_HOME` | 陷阱 1、2 由此发现 |

---

## 七、注意事项（非环境类）

### 7.1 磁盘占用参考

| 位置 | 占用 | 可清理性 |
|---|---|---|
| `D:\Rust\rustup` | 838 MB | 删了 Rust 就不能用 |
| `D:\Rust\cargo` | 485 MB | 同上 |
| `D:\Rust\bin` | 24 MB | 调试器二进制 |
| `G:\codex-workspace\tools\embedded-debugger-mcp` | 928 MB | **其中大部分是 `target/` 编译产物，可 `cargo clean`** |
| `G:\codex-workspace\vision-debug` | 531 MB | esp-who 克隆 + 编译产物，可 `idf.py fullclean` |

### 7.2 安全提示

`HKCU:\Environment` 中以明文存放着 API key（`OPENROUTER_API_KEY`、
`ZHIPUAI_API_KEY`）。这是 Windows 环境变量的常见用法，
但**任何 dump 环境变量的操作都会把它们打在日志里**。
如果这些日志会被分享或上传，建议轮换这两个 key。

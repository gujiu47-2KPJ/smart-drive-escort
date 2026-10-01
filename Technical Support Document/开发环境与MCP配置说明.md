# 开发环境与 MCP 配置说明

> 记录日期：2026-10-02
> 适用项目：ESP32-S3 端侧多模态融合主动安全头盔（智行护航）

本文记录本机开发环境中已完成的配置、一次重要的故障根因分析，以及待办事项。
目的是让以后遇到同类问题时不必从零排查。

---

## 一、已安装的 MCP 服务器

### 来源

本机为 ReasonIX 和 OpenCode 配置过一整套 MCP 服务器，配置文件位于：

- `C:\Users\gujiu\.config\opencode\opencode.jsonc`（ReasonIX 配置的镜像）
- `D:\opencode\config\opencode.jsonc`（同一份副本）

原配置共 10 个服务器。按"只保留对 ESP32 项目真正有用"的原则筛选后，
**只安装 1 个**：串口 MCP。

### 为什么只留串口 MCP

| 服务器 | 决定 | 原因 |
|---|---|---|
| `serial-mcp` | **安装** | 直接读写 UART，雷达/IMU 不用反复烧程序就能抓线上数据 |
| `stm32-analysis` | 不装 | STM32 专用，本项目是 ESP32 |
| `vscode-cube-mcp` | 不装 | STM32CubeMX 专用 |
| `git-mcp` | 不装 | 命令行 git 已覆盖，无额外能力 |
| `mcp-filesystem` | 不装 | 编辑器的原生文件工具已覆盖 |
| `github` | 不装 | 需要 token，当前无 GitHub 协作需求 |
| `desktop-commander` | 不装 | 上游配置里本来就是 `enabled: false`，功能与原生工具重复 |
| `fetch-mcp` | 不装 | 上游已因 API 变更禁用，且未适配 |
| `upstash` | 不装 | Redis 数据服务，与本项目无关 |

### 串口 MCP 详情

启动方式：

```
D:\opencode\venv-mcp1\Scripts\python.exe -m serial_mcp_server.server
```

运行环境：Python 3.12.10 + `mcp 1.29.0` + `pyserial 3.5`
（独立 venv，因为该服务器用 mcp 1.x API，与系统 Python 的 mcp 2.0.0 冲突）

安装前做过真实握手验证：`initialize` + `tools/list` 正常返回，
服务器标识 `serial-mcp-server v1.29.0`，共 **27 个工具**。

可用工具中与本项目直接相关的：

- `serial.list_ports` —— 列出串口
- `serial.open` / `serial.close` / `serial.connection_status`
- `serial.read` / `serial.readline` / `serial.read_until` / `serial.write` / `serial.flush`
- `serial.set_dtr` / `serial.set_rts` / `serial.pulse_dtr` / `serial.pulse_rts`
  —— ESP32 复位/进下载模式会用到
- `serial.spec.*` —— 协议模板（可把 AT6010 帧格式登记成 spec）
- `serial.trace.*` —— 调用轨迹，排障用

### 安装位置

写入 `C:\Users\gujiu\.codex\config.toml`：

```toml
[mcp_servers.serial]
command = 'D:\opencode\venv-mcp1\Scripts\python.exe'
args = ['-m', 'serial_mcp_server.server']
enabled = true
startup_timeout_sec = 20
tool_timeout_sec = 120
```

安装脚本：`G:\codex-workspace\tools\install-serial-mcp.ps1`
（先备份 → 已存在则跳过 → 追加 → 用 TOML 解析器校验 → 失败自动回滚）

配置备份：`G:\codex-workspace\backup\codex-config-20261002-141426\config.toml.bak`

**注意**：MCP 服务器在应用启动时加载，改完必须**重启 Codex** 才生效。
重启后在输入框输入 `/mcp` 可以看到 `serial` 是否已连接。

---

## 二、Codex 沙箱故障根因分析（重要）

### 症状

所有非提升权限的命令执行全部失败：

```
CreateProcess ... helper_unknown_error: setup refresh had errors
```

只有申请提升权限的命令能跑。表现为"Agent 突然不会用命令行"。

### 排查过程

日志位置：`C:\Users\gujiu\.codex\.sandbox\sandbox.<日期>.log`

关键行：

```
deny ACE failed on G:\codex-workspace\.git: open deny ACL target for update
setup refresh completed with errors
setup error: setup refresh had errors
```

### 根因

Codex 的 Windows 沙箱会在可写工作区内，对 `.git`、`.agents`、`.codex`、`.aws`
这类敏感目录写一条**拒绝写入的 ACL 条目**（deny ACE），以此保护它们不被改写。

写 ACL 需要 `WRITE_DAC` 权限，而 Windows 规则是：
**只有对象所有者，或持有 `FullControl` 且令牌已提权的账户才有 `WRITE_DAC`。**
普通 `Modify` 权限**不包含** `WRITE_DAC`。

而 `G:\codex-workspace\.git` 的所有者是：

```
TIANXUAN\CodexSandboxOffline     ← 沙箱账户
```

原因是 `.git` 由**在沙箱内运行的 git** 于 `2026-10-01 14:13:37` 创建，
创建者成为所有者。此后沙箱每次初始化都无法写那条 deny ACE，于是整体中止。

时间线完全吻合：

| 日志 | 结果 |
|---|---|
| `sandbox.2026-06-06.log` | 无 deny ACE 报错，初始化成功 |
| `sandbox.2026-09-30.log` | 无 deny ACE 报错，初始化成功 |
| `sandbox.2026-10-01.log` | **第 950 行首次出现** deny ACE 失败 |
| `sandbox.2026-10-02.log` | 每次尝试都失败 |

### 修复

把 `.git` 的所有者改回真实用户。脚本：

`G:\codex-workspace\tools\fix-sandbox-git-acl.ps1`

该脚本会自动请求管理员提权，只修改 `G:\codex-workspace\.git` 的所有者和 ACL，
并先把原 ACL 备份到 `G:\codex-workspace\backup\git-acl-<时间戳>\git-acl.txt`。

回滚命令：

```
icacls G:\codex-workspace /restore "G:\codex-workspace\backup\git-acl-<时间戳>\git-acl.txt"
```

### 修复结果

```
owner : TIANXUAN\gujiu        （原来：TIANXUAN\CodexSandboxOffline）
WRITE_DAC 验证 : True
setup refresh: processed 3 write roots; errors=[]      ← 零错误
setup_error.json → 自动消失
```

**后续注意**：如果将来在沙箱内重新 `git init`，`.git` 会再次变成沙箱账户所有，
同样的故障会复发。届时重跑一次上面的修复脚本即可。

---

## 三、环境体检发现的其他问题（未处理）

### 1. PATH 环境变量含多条失效路径

```
[ 4] C:\ST\STM32CubeCLT_1.18.0                          （软件已卸载）
[ 5] C:\ST\STM32CubeCLT_1.18.0\CMake\bin
[ 6] C:\ST\STM32CubeCLT_1.18.0\Ninja\bin
[ 7] C:\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin
[ 8] C:\ST\STM32CubeCLT_1.18.0\STLink-gdb-server\bin
[ 9] C:\ST\STM32CubeCLT_1.18.0\STM32CubeProgrammer\bin
[23] C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Ut   ← 路径被截断
[24] C:\Program Files\AskLink
[26] D:\MSYS2\mingw64\bin
[28] F:\yolov8 trainng\Git\cmd
[43] D:\opencode\app
```

第 23 条是被**截断的完整路径**，说明环境变量曾被手工编辑或写入异常。

修复脚本已备好（默认 dry-run，不写任何东西）：
`G:\codex-workspace\tools\repair-path-and-profile.ps1`

### 2. PowerShell 7 的 profile 缺少 BOM

- `C:\Users\gujiu\Documents\PowerShell\Microsoft.PowerShell_profile.ps1`
  —— UTF-8 **无 BOM**，含中文，被按 GBK 解码成乱码
- `C:\Users\gujiu\Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1`
  —— UTF-8 **有 BOM**，正常

后果：所有含中文的 .ps1 脚本在 Windows PowerShell 5.1 下会解析失败
（本次排查中就踩到过两次）。上面的修复脚本会把 PowerShell 7 那份
重写为"带 BOM + 纯 ASCII 内容"。

### 3. 磁盘空间

| 盘 | 可用 | 总计 |
|---|---|---|
| C: | 25.7 GB | 200.7 GB |
| D: | 106.9 GB | 450 GB |
| E: | 118.1 GB | 274 GB |
| G: | 905.4 GB | 1862 GB |

Codex 相关占用：

| 目录 | 大小 |
|---|---|
| `C:\Users\gujiu\.codex` | 0.84 GB |
| `C:\Users\gujiu\.cache\codex-runtimes` | 1.31 GB |
| `C:\Users\gujiu\.cache`（合计） | 2.91 GB |
| `C:\Users\gujiu\.espressif` | **6.79 GB** |
| `D:\Espressif` | 6.79 GB |

`C:\Users\gujiu\.espressif` 与 `D:\Espressif` 体积**完全相同**，
疑似重复安装，值得单独确认——这是比迁移 Codex 缓存收益更大的清理点。

---

## 四、待办

1. **重启 Codex**，输入 `/mcp` 确认 `serial` 已连接。
2. 决定是否清理 PATH 死条目（先跑 dry-run 看清单）。
3. 确认 `C:\Users\gujiu\.espressif` 是否为 `D:\Espressif` 的重复/联接。
4. 评估把 Codex 缓存迁到 E 盘（需要先关闭 Codex，分步验证）。

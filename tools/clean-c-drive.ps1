<#
  智行护航 —— C 盘安全清理脚本

  设计原则（安全第一）：
    1. 只删「白名单」里写死的路径，绝不通配、绝不递归上级目录
    2. 每一项删除前先打印路径、大小，再确认路径仍在白名单内
    3. 删除失败（占用/权限）只记录，不重试、不夺权
    4. 不碰任何用户数据目录（文档/桌面/视频/下载/聊天记录/项目代码）
    5. 全程写日志，删了什么、省了多少，事后可查

  用法：
    只预览不删除（不需要管理员）：
      pwsh -File "G:\codex-workspace\tools\clean-c-drive.ps1"
    真正执行（必须管理员）：
      pwsh -File "G:\codex-workspace\tools\clean-c-drive.ps1" -Apply
#>

param(
    [switch]$Apply,
    [string]$LogPath = "G:\codex-workspace\backup\cleanup-logs"
)

$ErrorActionPreference = 'Continue'
$U = $env:USERPROFILE

New-Item -ItemType Directory -Force -Path $LogPath | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logFile = Join-Path $LogPath "clean-c-drive.$stamp.log"

function Log([string]$m) {
    $line = "{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    Add-Content -LiteralPath $logFile -Value $line -Encoding UTF8
}

function FreeGB { (Get-PSDrive C).Free / 1GB }
function SizeOf($p) {
    if (-not (Test-Path -LiteralPath $p)) { return -1 }
    $i = Get-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
    if ($null -eq $i) { return -1 }
    if ($i.PSIsContainer) {
        return (Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    }
    return $i.Length
}

# ---------------------------------------------------------------
# 白名单：分类列出「可以删」和「绝对不删」
# ---------------------------------------------------------------

# A 类：纯缓存，程序会自己重建（风险≈0）
$cacheTargets = @(
    "$U\AppData\Local\NVIDIA",                                  # 显卡 DXCache / GLCache 着色器缓存
    "$U\AppData\Local\NVIDIA Corporation",                      # 同上
    "$U\AppData\Local\D3DSCache",                               # DirectX 着色器缓存
    "C:\Program Files\NVIDIA Corporation\Installer2",           # 显卡驱动安装包缓存
    "$U\AppData\Local\eim\offline_archives",                    # ESP-IDF 安装器离线包缓存（IDF 本体在 D 盘）
    "$U\AppData\Local\Microsoft\vscode-cpptools",               # VS Code C++ 智能感知数据库
    "$U\AppData\Roaming\Code\CachedExtensionVSIXs",             # 已下载的扩展安装包
    "$U\AppData\Roaming\Code\GPUCache",
    "$U\AppData\Roaming\Code\Crashpad",
    "$U\AppData\Roaming\Code\logs",
    "$U\AppData\Local\npm-cache",
    "$U\AppData\Roaming\npm-cache"
)

# B 类：系统级残留（需要管理员）
$systemTargets = @(
    "C:\`$WINDOWS.~BT",     # Windows 升级安装器临时目录
    "C:\`$WinREAgent"       # Windows 更新恢复代理临时目录
)

# D 类：浏览器 / 编辑器 / 系统日志缓存（同样会自动重建）
$cacheTargets2 = @(
    "$U\AppData\Local\Microsoft\Edge\User Data\Default\Cache",
    "$U\AppData\Local\Microsoft\Edge\User Data\Default\Code Cache",
    "$U\AppData\Local\Microsoft\Edge\User Data\Default\GPUCache",
    "$U\AppData\Local\Microsoft\Edge\User Data\Default\Service Worker\CacheStorage",
    "$U\AppData\Roaming\Code\Cache",
    "$U\AppData\Roaming\Code\GPUCache",
    "$U\AppData\Roaming\Code\DawnGraphiteCache",
    "$U\AppData\Roaming\Code\DawnWebGPUCache"
)

# 目录里「只删旧文件」的目标（保留最近 1 天，避免影响正在运行的程序）
$oldFileTargets = @(
    "$U\AppData\Local\Temp"
)

# 明确保护，永不触碰（写在这里是为了让后来的人看清楚边界）
$protected = @(
    "C:\Windows", "C:\Windows\Installer", "C:\Windows\System32\DriverStore",
    "C:\eSupport",                                          # 同方 FA608PM 出厂驱动包，未纳入本次清理
    "$U\Documents", "$U\Desktop", "$U\Videos", "$U\Downloads", "$U\Pictures",
    "$U\.cache\codex-runtimes",                             # Codex 自身运行时
    "$U\.espressif",                                        # ESP-IDF 工具链
    "$U\AppData\Local\Arduino15",                           # Arduino 平台，交给用户决定
    "$U\AppData\Roaming\Tencent", "$U\AppData\Roaming\QQ",
    "$U\AppData\Roaming\EA", "$U\AppData\Roaming\Code\User"  # 用户配置与数据
)

Log "================ C 盘清理开始 ================"
Log ("模式: " + $(if ($Apply) { "执行删除" } else { "预览（不删除）" }))
Log ("开始前 C 盘可用: {0:N2} GB" -f (FreeGB))

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log ("当前会话是否管理员: " + $isAdmin)

if ($Apply -and -not $isAdmin) {
    Log "错误：-Apply 需要管理员权限，已中止（未做任何修改）。"
    exit 1
}

$totalFreed = 0

# ---------- A 类：缓存 ----------
Log ""
Log "---- A 类：可重建缓存 ----"
foreach ($t in $cacheTargets) {
    $full = [IO.Path]::GetFullPath($t)
    $size = SizeOf $full
    if ($size -lt 0) { Log ("跳过(不存在): " + $full); continue }

    # 安全校验：目标不能是白名单项本身的上级
    $bad = $false
    foreach ($p in $protected) { if ($full -eq [IO.Path]::GetFullPath($p) -or $full.StartsWith([IO.Path]::GetFullPath($p) + '\')) { $bad = $true } }
    if ($bad) { Log ("拒绝(命中保护名单): " + $full); continue }

    Log ("删除 {0,9:N2} MB  {1}" -f ($size/1MB), $full)
    if ($Apply) {
        try {
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
            $left = SizeOf $full
            if ($left -lt 0) { $totalFreed += $size; Log "    成功" }
            else { Log ("    部分成功，剩余 {0:N2} MB（可能被占用）" -f ($left/1MB)); $totalFreed += ($size - $left) }
        } catch {
            Log ("    失败: " + $_.Exception.Message)
        }
    }
}

# ---------- D 类：浏览器 / 编辑器 / 系统日志缓存 ----------
Log ""
Log "---- D 类：浏览器与编辑器缓存 ----"
foreach ($t in $cacheTargets2) {
    $full = [IO.Path]::GetFullPath($t)
    $size = SizeOf $full
    if ($size -lt 0) { Log ("跳过(不存在): " + $full); continue }

    $bad = $false
    foreach ($p in $protected) { if ($full -eq [IO.Path]::GetFullPath($p) -or $full.StartsWith([IO.Path]::GetFullPath($p) + '\')) { $bad = $true } }
    if ($bad) { Log ("拒绝(命中保护名单): " + $full); continue }

    Log ("删除 {0,9:N2} MB  {1}" -f ($size/1MB), $full)
    if ($Apply) {
        try {
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
            $left = SizeOf $full
            if ($left -lt 0) { $totalFreed += $size; Log "    成功" }
            else { Log ("    部分成功，剩余 {0:N2} MB（可能被占用）" -f ($left/1MB)); $totalFreed += ($size - $left) }
        } catch {
            Log ("    失败: " + $_.Exception.Message)
        }
    }
}

# ---------- E 类：临时目录里「超过 1 天」的旧文件 ----------
Log ""
Log "---- E 类：临时目录旧文件（只删超过 1 天的，正在使用的自动跳过） ----"
foreach ($t in $oldFileTargets) {
    if (-not (Test-Path -LiteralPath $t)) { Log ("跳过(不存在): " + $t); continue }
    $cutoff = (Get-Date).AddDays(-1)
    $items = Get-ChildItem -LiteralPath $t -Force -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cutoff }
    $s = 0
    foreach ($i in $items) { $v = SizeOf $i.FullName; if ($v -gt 0) { $s += $v } }
    Log ("清理 {0,9:N2} MB / {1} 项  {2}" -f ($s/1MB), $items.Count, $t)
    if ($Apply) {
        $ok = 0; $skip = 0
        foreach ($i in $items) {
            try { Remove-Item -LiteralPath $i.FullName -Recurse -Force -ErrorAction Stop; $ok++ } catch { $skip++ }
        }
        Log ("    已删 $ok 项，跳过 $skip 项（被占用或无权限）")
        $totalFreed += $s
    }
}

# ---------- B 类：系统残留 ----------
Log ""
Log "---- B 类：系统更新残留 ----"
if (-not $isAdmin) {
    Log "跳过（需要管理员）"
} else {
    foreach ($t in $systemTargets) {
        $size = SizeOf $t
        if ($size -lt 0) { Log ("跳过(不存在): " + $t); continue }
        Log ("删除 {0,9:N2} MB  {1}" -f ($size/1MB), $t)
        if ($Apply) {
            try {
                Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction Stop
                $totalFreed += $size; Log "    成功"
            } catch {
                Log ("    失败: " + $_.Exception.Message)
            }
        }
    }
}

# ---------- C 类：休眠文件（最大单项） ----------
Log ""
Log "---- C 类：休眠文件 hiberfil.sys ----"
$hib = Get-Item C:\hiberfil.sys -Force -ErrorAction SilentlyContinue
if ($hib) {
    Log ("当前 hiberfil.sys 占用: {0:N2} GB" -f ($hib.Length/1GB))
    if (-not $isAdmin) {
        Log "跳过（需要管理员）"
    } elseif ($Apply) {
        powercfg /h off 2>&1 | ForEach-Object { Log ("    " + $_) }
        Start-Sleep -Seconds 2
        $after = Test-Path C:\hiberfil.sys
        Log ("执行 powercfg /h off 完成，hiberfil.sys 仍存在: " + $after)
        Log "    还原命令: powercfg /h on（会重新占用约 12.5 GB）"
    } else {
        Log "预览：将执行 powercfg /h off（可随时用 powercfg /h on 还原）"
    }
}

# ---------- 收尾 ----------
Log ""
$end = FreeGB
Log ("结束，C 盘可用: {0:N2} GB" -f $end)
if ($Apply) { Log ("本次释放约 {0:N2} GB" -f ($totalFreed/1GB)) }
Log ("日志: " + $logFile)
Log "================ 结束 ================"

<#
  智行护航 —— 需要「管理员权限」的一次性环境修复脚本

  为什么需要管理员：
    - 开启 Windows 长路径要写 HKLM
    - 清理 $WINDOWS.~BT / $WinREAgent 属于系统目录
    - 调整休眠文件、删除 C:\eSupport 都需要管理员

  用法（二选一）：
    1) 开始菜单搜 "PowerShell" -> 右键 "以管理员身份运行"，然后粘贴：
         Set-ExecutionPolicy -Scope Process Bypass -Force
         & "G:\codex-workspace\tools\admin-fix.ps1"
    2) 在 VS Code 终端里执行：
         Start-Process pwsh -Verb RunAs -ArgumentList '-NoExit','-File','G:\codex-workspace\tools\admin-fix.ps1'

  安全设计：
    - 每一步都先打印「将要做什么」，默认回车 = 跳过，不执行任何操作
    - 不做不可逆操作；休眠文件可以用 powercfg /h on 还原
    - 删除前会再次确认，并打印删除前后的可用空间
#>

$ErrorActionPreference = 'Continue'

function Show-Free([string]$tag) {
    $f = (Get-PSDrive C).Free
    Write-Host ("[{0}] C 盘可用: {1:N2} GB" -f $tag, ($f / 1GB)) -ForegroundColor Cyan
}

function Confirm-Step([string]$title, [string]$detail) {
    Write-Host ""
    Write-Host ("=== " + $title + " ===") -ForegroundColor Yellow
    Write-Host $detail
    $a = Read-Host "执行吗？输入 y 执行，直接回车跳过"
    return ($a -eq 'y' -or $a -eq 'Y')
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "当前不是管理员，脚本无法完成系统级修改。" -ForegroundColor Red
    Write-Host "请用「以管理员身份运行」重新打开 PowerShell 后执行本脚本。" -ForegroundColor Red
    exit 1
}

Show-Free '开始'

# ---------------------------------------------------------------
# 1. 开启 Windows 长路径支持（对 ESP-IDF / Git 深目录重要）
#    作用：允许超过 260 字符的路径
#    还原：把值改回 0
# ---------------------------------------------------------------
if (Confirm-Step "1/4 开启长路径支持（HKLM，需要管理员）" "将设置 HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`n作用：ESP-IDF 构建路径很深，超过 260 字符会失败。`n风险：极低，且可随时改回 0。") {
    $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem'
    $old = (Get-ItemProperty -Path $k -Name LongPathsEnabled -ErrorAction SilentlyContinue).LongPathsEnabled
    Set-ItemProperty -Path $k -Name LongPathsEnabled -Value 1 -Type DWord
    $new = (Get-ItemProperty -Path $k -Name LongPathsEnabled).LongPathsEnabled
    Write-Host "LongPathsEnabled: $old -> $new (重启后对所有程序生效)" -ForegroundColor Green
} else { Write-Host "已跳过" -ForegroundColor DarkGray }

# ---------------------------------------------------------------
# 2. 清理 Windows 更新残留
#    $WINDOWS.~BT  : 升级安装器的临时目录
#    $WinREAgent   : 更新恢复代理的临时目录
#    两者都是「更新流程的中间产物」，更新完成后再无用途。
#    注意：若当前有正在进行的 Windows 更新，请先重启完成更新再执行。
# ---------------------------------------------------------------
$leftovers = @('C:\$WINDOWS.~BT', 'C:\$WinREAgent')
$sum = 0
foreach ($p in $leftovers) {
    if (Test-Path -LiteralPath $p) {
        $sum += (Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    }
}
if (Confirm-Step "2/4 清理 Windows 更新残留（约 {0:N2} GB）" -f ($sum/1GB) "将删除：`n  C:\`$WINDOWS.~BT`n  C:\`$WinREAgent`n风险：低。这是 Windows 更新/升级留下的临时目录，正常情况下系统会自行回收。`n前提：当前没有未完成的 Windows 更新（有的话先重启一次）。") {
    foreach ($p in $leftovers) {
        if (Test-Path -LiteralPath $p) {
            try {
                Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop
                Write-Host "已删除 $p" -ForegroundColor Green
            } catch {
                Write-Host "删除失败 $p : $($_.Exception.Message)" -ForegroundColor Red
            }
        } else { Write-Host "不存在，跳过: $p" -ForegroundColor DarkGray }
    }
    Show-Free '清理后'
} else { Write-Host "已跳过" -ForegroundColor DarkGray }

# ---------------------------------------------------------------
# 3. 休眠文件 hiberfil.sys —— C 盘最大的一块可回收空间（约 12.5 GB）
#    三个选项，默认不改：
#      reduced : 只保留「快速启动」，保持休眠文件约一半大小（省 ~6 GB）
#      off     : 完全关闭休眠和快速启动（省 ~12.5 GB）
#    还原：powercfg /h on
# ---------------------------------------------------------------
$hib = Get-Item C:\hiberfil.sys -Force -ErrorAction SilentlyContinue
if ($hib) {
    Write-Host ""
    Write-Host ("=== 3/4 休眠文件 hiberfil.sys 当前占用 {0:N2} GB ===" -f ($hib.Length/1GB)) -ForegroundColor Yellow
    Write-Host "  0 = 不改（保留休眠 + 快速启动）"
    Write-Host "  1 = 只保留快速启动，文件缩小约一半，可省约 6 GB"
    Write-Host "  2 = 完全关闭休眠和快速启动，可省约 12.5 GB"
    $c = Read-Host "请选择 0/1/2（直接回车=0）"
    if ($c -eq '1') {
        powercfg /h /type reduced
        Write-Host "已切换为 reduced（保留快速启动）" -ForegroundColor Green
        Show-Free '处理后'
    } elseif ($c -eq '2') {
        powercfg /h off
        Write-Host "已完全关闭休眠（还原命令：powercfg /h on）" -ForegroundColor Green
        Show-Free '处理后'
    } else { Write-Host "已跳过" -ForegroundColor DarkGray }
} else { Write-Host "未发现 hiberfil.sys，跳过" -ForegroundColor DarkGray }

# ---------------------------------------------------------------
# 4. C:\eSupport —— OEM 厂商预装的驱动备份（约 6.1 GB）
#    这是电脑厂商（如机械革命/蓝天）首次开机时放的驱动包。
#    删掉不影响系统运行，但以后重装系统/装驱动要回官网下载。
#    默认不删，必须手动输入 YES 才会执行。
# ---------------------------------------------------------------
$es = 'C:\eSupport'
if (Test-Path -LiteralPath $es) {
    $sz = (Get-ChildItem -LiteralPath $es -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    Write-Host ""
    Write-Host ("=== 4/4 C:\eSupport 厂商驱动备份 {0:N2} GB ===" -f ($sz/1GB)) -ForegroundColor Yellow
    Write-Host "删除不影响系统运行，但以后装驱动需要去官网重新下载。"
    $c = Read-Host "确认删除请输入 YES（其它任何输入=跳过）"
    if ($c -eq 'YES') {
        try {
            Remove-Item -LiteralPath $es -Recurse -Force -ErrorAction Stop
            Write-Host "已删除 C:\eSupport" -ForegroundColor Green
            Show-Free '处理后'
        } catch {
            Write-Host "删除失败: $($_.Exception.Message)" -ForegroundColor Red
        }
    } else { Write-Host "已跳过" -ForegroundColor DarkGray }
} else { Write-Host "未发现 C:\eSupport，跳过" -ForegroundColor DarkGray }

Write-Host ""
Write-Host "全部步骤结束。" -ForegroundColor Cyan
Show-Free '最终'

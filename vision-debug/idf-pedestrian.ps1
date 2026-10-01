# ============================================================
#  ESP-WHO 行人检测 —— 一键构建/烧录脚本
#
#  为什么需要这个脚本：
#    1) ESP-WHO 的 tools/bsp_ext.py 扩展要求 SDKCONFIG_DEFAULTS 和
#       DETECT_MODEL 这两个变量在【每一条】idf.py 命令里都存在，
#       不只是 set-target。
#    2) PowerShell 会把 "-DSDKCONFIG_DEFAULTS=xxx" 这种参数拆散，
#       导致 idf.py 报 "No such option: -D"。
#       改用环境变量传递可以绕开这个问题。
#
#  用法：
#    .\idf-pedestrian.ps1 build                  # 编译
#    .\idf-pedestrian.ps1 -Port COM9 flash monitor   # 烧录并看日志
#    .\idf-pedestrian.ps1 fullclean              # 清理
#
#  说明：本脚本只设置环境变量并调用 idf.py，不修改工程源码。
# ============================================================

param(
    [string]$Port = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = 'Stop'

# ---- ESP-IDF 环境（路径取自本机 VS Code ESP-IDF 插件配置）----
$IdfPath     = 'D:\esp\v6.0.2\esp-idf'
$IdfToolsPath = 'D:\Espressif'

if (-not (Test-Path -LiteralPath "$IdfPath\export.ps1")) {
    throw "找不到 ESP-IDF export.ps1：$IdfPath\export.ps1"
}

$env:IDF_PATH       = $IdfPath
$env:IDF_TOOLS_PATH = $IdfToolsPath
. "$IdfPath\export.ps1" | Out-Null

# ---- ESP-WHO 扩展与工程定位 ----
# 本脚本位于 …\vision-debug\ 下，esp-who 是它的同级子目录
$repoRoot = $PSScriptRoot
$env:IDF_EXTRA_ACTIONS_PATH = Join-Path $repoRoot 'esp-who\tools'

# ---- 这两个必须每次都给，否则扩展会直接报错退出 ----
$env:SDKCONFIG_DEFAULTS = 'sdkconfig.bsp.esp32_s3_eye_noglib'
$env:DETECT_MODEL       = 'pedestrian_detect'

$exampleDir = Join-Path $repoRoot 'esp-who\examples\object_detect'
if (-not (Test-Path -LiteralPath $exampleDir)) {
    throw "找不到示例目录：$exampleDir"
}
Set-Location $exampleDir

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) {
    $IdfArgs = @('build')
}

Write-Host ("[idf-pedestrian] BSP    = {0}" -f $env:SDKCONFIG_DEFAULTS)
Write-Host ("[idf-pedestrian] MODEL  = {0}" -f $env:DETECT_MODEL)
Write-Host ("[idf-pedestrian] 目录   = {0}" -f $exampleDir)
Write-Host ("[idf-pedestrian] 命令   = idf.py {0}" -f ($IdfArgs -join ' '))
Write-Host ''

if ($Port) {
    idf.py -p $Port @IdfArgs
} else {
    idf.py @IdfArgs
}

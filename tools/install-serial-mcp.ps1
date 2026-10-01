# ============================================================
#  Install the serial MCP server into Codex's config.toml
#
#  Order of operations (safety first):
#    1. back up the current config.toml verbatim
#    2. refuse to touch it if [mcp_servers.serial] already exists
#    3. append the new table (never rewrite existing content)
#    4. re-parse the file with a real TOML parser to prove it is valid
#    5. if parsing fails, restore the backup automatically
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\install-serial-mcp.ps1
# ============================================================

$ErrorActionPreference = 'Stop'

$configPath  = Join-Path $HOME '.codex\config.toml'
$fragmentPath = 'G:\codex-workspace\tools\mcp-serial-fragment.toml'
$pythonExe   = 'D:\opencode\venv-mcp1\Scripts\python.exe'

function Say([string]$Text) { Write-Host $Text }

Say '=== 1. preconditions ==='
if (-not (Test-Path -LiteralPath $configPath)) { throw "config not found: $configPath" }
if (-not (Test-Path -LiteralPath $fragmentPath)) { throw "fragment not found: $fragmentPath" }
if (-not (Test-Path -LiteralPath $pythonExe)) { throw "python not found: $pythonExe" }
Say ("  config   : {0}" -f $configPath)
Say ("  fragment : {0}" -f $fragmentPath)

$existing = Get-Content -LiteralPath $configPath -Raw
if ($existing -match '(?m)^\s*\[mcp_servers\.serial\]') {
    Say ''
    Say '  [mcp_servers.serial] ALREADY PRESENT -> nothing to do.'
    Say '  (edit that block directly to change settings)'
    exit 0
}
Say '  [mcp_servers.serial] not present -> will append.'

Say ''
Say '=== 2. backup ==='
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path 'G:\codex-workspace\backup' ("codex-config-" + $stamp)
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
$backupFile = Join-Path $backupDir 'config.toml.bak'
Copy-Item -LiteralPath $configPath -Destination $backupFile -Force
$beforeHash = (Get-FileHash -LiteralPath $backupFile -Algorithm SHA256).Hash
Say ("  backup dir : {0}" -f $backupDir)
Say ("  backup file: {0}" -f $backupFile)
Say ("  sha256     : {0}" -f $beforeHash)

Say ''
Say '=== 3. append ==='
$fragment = Get-Content -LiteralPath $fragmentPath -Raw
$newContent = $existing.TrimEnd() + "`r`n" + $fragment + "`r`n"
[System.IO.File]::WriteAllText($configPath, $newContent, (New-Object System.Text.UTF8Encoding($false)))
Say ("  appended {0} chars; file is now {1} bytes" -f $fragment.Length, (Get-Item -LiteralPath $configPath).Length)

Say ''
Say '=== 4. validate with a real TOML parser ==='
$validator = Join-Path $env:TEMP 'validate_codex_toml.py'
@'
import sys, tomllib
p = sys.argv[1]
with open(p, "rb") as fh:
    data = tomllib.load(fh)
servers = data.get("mcp_servers", {})
print("TOML OK")
print("mcp_servers keys:", sorted(servers.keys()))
if "serial" in servers:
    s = servers["serial"]
    print("serial.command:", s.get("command"))
    print("serial.args   :", s.get("args"))
    print("serial.enabled:", s.get("enabled"))
'@ | Set-Content -LiteralPath $validator -Encoding UTF8

$validateOutput = & $pythonExe $validator $configPath 2>&1
$validateExit = $LASTEXITCODE
$validateOutput | ForEach-Object { Say ('  ' + $_) }

if ($validateExit -ne 0) {
    Say ''
    Say '!!! TOML PARSE FAILED - restoring backup !!!'
    Copy-Item -LiteralPath $backupFile -Destination $configPath -Force
    $afterHash = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
    Say ("  restored, sha256 now: {0}" -f $afterHash)
    Say ("  matches original    : {0}" -f ($afterHash -eq $beforeHash))
    exit 1
}

Say ''
Say '=== 5. done ==='
$finalHash = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
Say ("  final sha256 : {0}" -f $finalHash)
Say ("  backup sha256: {0}" -f $beforeHash)
Say ''
Say '  Next: restart the Codex app, then type /mcp in the composer'
Say '        and confirm "serial" is listed and connected.'
Say ''
Say '  Rollback any time with:'
Say ("    Copy-Item -LiteralPath '{0}' -Destination '{1}' -Force" -f $backupFile, $configPath)

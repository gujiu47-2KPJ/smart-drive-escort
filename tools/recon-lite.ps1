# ============================================================
#  READ-ONLY, FAST reconnaissance script
#  Scope: MCP configs + Codex cache locations + PATH/profile health
#
#  Does NOT modify, delete, or move anything.
#  Runs in tens of seconds (no deep recursive size walks).
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\recon-lite.ps1
#
#  Report: G:\codex-workspace\tools\_recon-lite.txt
# ============================================================

$ErrorActionPreference = 'Continue'
$script:ReportLines = New-Object System.Collections.Generic.List[string]
function Add-OutLine([string]$Text) { $script:ReportLines.Add([string]$Text) }
function Add-SectionHead([string]$Text) {
    Add-OutLine ''
    Add-OutLine ('=' * 66)
    Add-OutLine ('  ' + $Text)
    Add-OutLine ('=' * 66)
}
function Add-Pair([string]$Label, $Value) { Add-OutLine ("  {0,-30}: {1}" -f $Label, $Value) }
function Get-TopLevelSizeText([string]$FolderPath) {
    # Only sums DIRECT top-level entries; avoids a full recursive walk of C:
    $sum = 0
    Get-ChildItem -LiteralPath $FolderPath -Force -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.PSIsContainer) {
            $s = (Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force -ErrorAction SilentlyContinue |
                  Measure-Object -Property Length -Sum).Sum
            if ($s) { $sum += $s }
        } else {
            $sum += $_.Length
        }
    }
    return ("{0} GB" -f [math]::Round($sum / 1GB, 3))
}

Add-SectionHead '0. BASIC INFO'
Add-Pair 'time' (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
Add-Pair 'PSVersion' $PSVersionTable.PSVersion.ToString()
Add-Pair 'PSEdition' $PSVersionTable.PSEdition
Add-Pair 'OutputEncoding' ([Console]::OutputEncoding.WebName)
Add-Pair 'CurrentDir' (Get-Location).Path

Add-SectionHead '1. DRIVE FREE SPACE'
Get-PSDrive -PSProvider FileSystem | ForEach-Object {
    if ($null -ne $_.Free -and $_.Used -gt 0) {
        Add-Pair $_.Name ("{0}  free {1} GB / total {2} GB" -f $_.Root,
            [math]::Round($_.Free / 1GB, 2), [math]::Round(($_.Used + $_.Free) / 1GB, 2))
    }
}

Add-SectionHead '2. POWERSHELL PROFILES'
foreach ($profilePath in @(
        (Join-Path $HOME 'Documents\PowerShell\Microsoft.PowerShell_profile.ps1'),
        (Join-Path $HOME 'Documents\PowerShell\profile.ps1'),
        (Join-Path $HOME 'Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1'),
        (Join-Path $HOME 'Documents\WindowsPowerShell\profile.ps1'),
        (Join-Path $HOME '.config\powershell\profile.ps1'))) {
    if (Test-Path -LiteralPath $profilePath) {
        $rawBytes = [System.IO.File]::ReadAllBytes($profilePath)
        $hasBom = ($rawBytes.Length -ge 3 -and $rawBytes[0] -eq 0xEF -and $rawBytes[1] -eq 0xBB -and $rawBytes[2] -eq 0xBF)
        Add-OutLine ''
        Add-OutLine ("  [FOUND] {0}" -f $profilePath)
        Add-Pair 'size bytes' $rawBytes.Length
        Add-Pair 'has UTF8 BOM' $hasBom
        Add-OutLine '  ---- content begin ----'
        Get-Content -LiteralPath $profilePath -ErrorAction SilentlyContinue | ForEach-Object { Add-OutLine ("  | " + $_) }
        Add-OutLine '  ---- content end ----'
    } else {
        Add-OutLine ("  [none]  {0}" -f $profilePath)
    }
}

Add-SectionHead '3. ENVIRONMENT'
Add-Pair 'HOME' $HOME
Add-Pair 'USERPROFILE' $env:USERPROFILE
Add-Pair 'CODEX_HOME' $env:CODEX_HOME
foreach ($envName in @('XDG_CACHE_HOME','IDF_PATH','IDF_TOOLS_PATH','TEMP','TMP',
                       'NPM_CONFIG_CACHE','PIP_CACHE_DIR','HF_HOME','ELECTRON_USER_DATA')) {
    Add-Pair $envName ([Environment]::GetEnvironmentVariable($envName))
}
Add-OutLine ''
Add-OutLine '  --- PATH entries (MISSING ones indicate a broken PATH) ---'
$pathIndex = 0
foreach ($pathSegment in ($env:PATH -split ';')) {
    $pathIndex++
    if ([string]::IsNullOrWhiteSpace($pathSegment)) { Add-OutLine ("  [{0,2}] <empty>" -f $pathIndex); continue }
    $pathExists = Test-Path -LiteralPath $pathSegment
    Add-OutLine ("  [{0,2}] {1} {2}" -f $pathIndex, $(if ($pathExists) { '[OK]     ' } else { '[MISSING]' }), $pathSegment)
}
Add-Pair 'PATH char count' $env:PATH.Length

Add-SectionHead '4. KEY DIRECTORY SIZES (shallow, fast)'
foreach ($sizeTarget in @("$HOME\.codex", "$HOME\.cache\codex-runtimes", "$HOME\.cache",
                          "$HOME\.espressif", "D:\Espressif", "D:\esp")) {
    if (Test-Path -LiteralPath $sizeTarget) {
        Add-Pair $sizeTarget (Get-TopLevelSizeText $sizeTarget)
    } else {
        Add-Pair $sizeTarget '(does not exist)'
    }
}

Add-SectionHead '5. MCP CONFIG CANDIDATES'
foreach ($mcpPath in @(
        "$env:APPDATA\Code\User\settings.json",
        "$env:APPDATA\Code\User\mcp.json",
        "$env:APPDATA\Code\mcp.json",
        "$env:APPDATA\Claude\claude_desktop_config.json",
        "$env:APPDATA\claude_desktop_config.json",
        "$HOME\.claude.json",
        "$HOME\.claude\settings.json",
        "$HOME\.claude\mcp.json",
        "$HOME\.codex\config.toml",
        "$HOME\.cursor\mcp.json",
        "$env:APPDATA\Cursor\User\settings.json",
        "$HOME\.gemini\settings.json")) {
    if (Test-Path -LiteralPath $mcpPath) {
        Add-OutLine ("  [EXISTS] {0}   ({1} bytes)" -f $mcpPath, (Get-Item -LiteralPath $mcpPath).Length)
    } else {
        Add-OutLine ("  [none]   {0}" -f $mcpPath)
    }
}
Add-OutLine ''
Add-OutLine '  --- VS Code settings.json lines mentioning mcp ---'
$codeSettingsPath = "$env:APPDATA\Code\User\settings.json"
if (Test-Path -LiteralPath $codeSettingsPath) {
    $mcpHits = Select-String -LiteralPath $codeSettingsPath -Pattern 'mcp' -SimpleMatch -ErrorAction SilentlyContinue
    if ($mcpHits) { $mcpHits | ForEach-Object { Add-OutLine ("  L{0}: {1}" -f $_.LineNumber, $_.Line.Trim()) } }
    else { Add-OutLine '  (no match)' }
} else { Add-OutLine '  (settings.json not found)' }

Add-SectionHead '6. MCP SERVER INSTALL TRACES'
foreach ($mcpDir in @('G:\.serial_mcp','G:\.omo','G:\.reasonix','G:\.codegraph','G:\.mcp')) {
    if (Test-Path -LiteralPath $mcpDir) {
        Add-OutLine ''
        Add-OutLine ("  [EXISTS] {0}" -f $mcpDir)
        Get-ChildItem -LiteralPath $mcpDir -Force -ErrorAction SilentlyContinue |
            Select-Object -First 30 |
            ForEach-Object {
                $kind = '     '
                if ($_.PSIsContainer) { $kind = '[DIR]' }
                Add-OutLine ("      {0} {1}" -f $kind, $_.Name)
            }
    } else {
        Add-OutLine ("  [none]   {0}" -f $mcpDir)
    }
}
Add-OutLine ''
Add-OutLine '  --- global npm packages ---'
$npmRoot = "$env:APPDATA\npm\node_modules"
if (Test-Path -LiteralPath $npmRoot) {
    Get-ChildItem -LiteralPath $npmRoot -Directory -Force -ErrorAction SilentlyContinue |
        Select-Object -First 80 | ForEach-Object { Add-OutLine ("    " + $_.Name) }
} else { Add-OutLine '    (no global npm dir)' }
Add-OutLine ''
Add-OutLine '  --- npx cache folders ---'
$npxCacheRoot = "$env:LOCALAPPDATA\npm-cache\_npx"
if (Test-Path -LiteralPath $npxCacheRoot) {
    Get-ChildItem -LiteralPath $npxCacheRoot -Directory -Force -ErrorAction SilentlyContinue |
        Select-Object -First 80 | ForEach-Object { Add-OutLine ("    " + $_.Name) }
} else { Add-OutLine '    (no npx cache)' }

Add-SectionHead '7. G DRIVE TOP LEVEL'
Get-ChildItem -LiteralPath 'G:\' -Force -ErrorAction SilentlyContinue |
    Select-Object -First 80 |
    ForEach-Object {
        $kind = '     '
        if ($_.PSIsContainer) { $kind = '[DIR]' }
        Add-OutLine ("  {0} {1}" -f $kind, $_.Name)
    }

Add-SectionHead '8. END'
Add-OutLine '  Nothing was modified.'

$reportPath = 'G:\codex-workspace\tools\_recon-lite.txt'
$script:ReportLines -join "`r`n" | Set-Content -LiteralPath $reportPath -Encoding UTF8
$script:ReportLines | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("Report written to: " + $reportPath)

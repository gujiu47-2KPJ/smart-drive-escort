# ============================================================
#  READ-ONLY: verify Codex runtime dependencies actually exist
#  (the paths reported by load_workspace_dependencies)
#
#  Rationale: if the exec helper cannot find its own bundled
#  runtime, process creation fails at the setup stage with
#  exactly the error we are seeing.
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\check-runtime-deps.ps1
#
#  Report: G:\codex-workspace\tools\_runtime-deps.txt
# ============================================================

$ErrorActionPreference = 'Continue'
$script:ReportLines = New-Object System.Collections.Generic.List[string]
function Add-OutLine([string]$Text) { $script:ReportLines.Add([string]$Text) }
function Add-SectionHead([string]$Text) {
    Add-OutLine ''
    Add-OutLine ('=' * 70)
    Add-OutLine ('  ' + $Text)
    Add-OutLine ('=' * 70)
}

$runtimeRoot = Join-Path $HOME '.cache\codex-runtimes\codex-primary-runtime'

Add-SectionHead '0. CONTEXT'
Add-OutLine ("  time             : {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Add-OutLine ("  runtime root     : {0}" -f $runtimeRoot)
Add-OutLine ("  runtime root exists: {0}" -f (Test-Path -LiteralPath $runtimeRoot))

Add-SectionHead '1. EXPECTED DEPENDENCY PATHS'
$expected = @(
    'dependencies\native\git\cmd\git.exe',
    'dependencies\node\bin\node.exe',
    'dependencies\node\node_modules',
    'dependencies\bin\fallback\pnpm.cmd',
    'dependencies\python\python.exe',
    'dependencies\python',
    'dependencies\bin\override',
    'dependencies\bin\fallback'
)
foreach ($rel in $expected) {
    $full = Join-Path $runtimeRoot $rel
    if (Test-Path -LiteralPath $full) {
        $item = Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
        $sizeText = ''
        if ($item -and -not $item.PSIsContainer) { $sizeText = ("  ({0} bytes)" -f $item.Length) }
        Add-OutLine ("  [OK]      {0}{1}" -f $full, $sizeText)
    } else {
        Add-OutLine ("  [MISSING] {0}" -f $full)
    }
}

Add-SectionHead '2. RUNTIME ROOT BREAKDOWN (depth 2)'
if (Test-Path -LiteralPath $runtimeRoot) {
    Get-ChildItem -LiteralPath $runtimeRoot -Recurse -Depth 2 -Force -ErrorAction SilentlyContinue |
        Select-Object -First 80 |
        ForEach-Object {
            $kind = '     '
            if ($_.PSIsContainer) { $kind = '[DIR]' }
            Add-OutLine ("  {0} {1}" -f $kind, $_.FullName.Substring($runtimeRoot.Length))
        }
}

Add-SectionHead '3. DRIVE TYPE AND FREE SPACE'
Add-OutLine '  WMI Win32_LogicalDisk:'
Get-CimInstance -ClassName Win32_LogicalDisk -ErrorAction SilentlyContinue |
    ForEach-Object {
        $driveTypeText = 'unknown'
        switch ($_.DriveType) {
            0 { $driveTypeText = 'Unknown' }
            1 { $driveTypeText = 'NoRootDir' }
            2 { $driveTypeText = 'Removable' }
            3 { $driveTypeText = 'Fixed' }
            4 { $driveTypeText = 'Network' }
            5 { $driveTypeText = 'CDROM' }
            6 { $driveTypeText = 'RAMDisk' }
        }
        $freeGB = ''
        if ($null -ne $_.FreeSpace) { $freeGB = ('{0} GB' -f [math]::Round($_.FreeSpace / 1GB, 2)) }
        Add-OutLine ("    {0}  type={1,-10} fs={2,-6} free={3}" -f $_.DeviceID, $driveTypeText, $_.FileSystem, $freeGB)
    }

Add-OutLine ''
Add-OutLine '  Get-Volume (write/health/remaining):'
Get-Volume -ErrorAction SilentlyContinue |
    Where-Object { $_.DriveLetter } |
    ForEach-Object {
        Add-OutLine ("    {0}:  fs={1,-6} health={2,-8} sizeRemaining={3} GB" -f `
            $_.DriveLetter, $_.FileSystem, $_.HealthStatus,
            [math]::Round($_.SizeRemaining / 1GB, 2))
    }

Add-SectionHead '4. TEMP DIR WRITABILITY (exec helper writes here)'
foreach ($tempCandidate in @($env:TEMP, $env:TMP, 'C:\Windows\Temp', "$HOME\AppData\Local\Temp")) {
    if ([string]::IsNullOrWhiteSpace($tempCandidate)) { continue }
    if (-not (Test-Path -LiteralPath $tempCandidate)) {
        Add-OutLine ("  [MISSING] {0}" -f $tempCandidate)
        continue
    }
    $probeFile = Join-Path $tempCandidate ('codex-write-probe-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [System.IO.File]::WriteAllText($probeFile, 'probe')
        $content = [System.IO.File]::ReadAllText($probeFile)
        Remove-Item -LiteralPath $probeFile -Force -ErrorAction SilentlyContinue
        if ($content -eq 'probe') {
            Add-OutLine ("  [WRITABLE] {0}" -f $tempCandidate)
        } else {
            Add-OutLine ("  [READBACK MISMATCH] {0}" -f $tempCandidate)
        }
    } catch {
        Add-OutLine ("  [NOT WRITABLE] {0}  -> {1}" -f $tempCandidate, $_.Exception.Message)
    }
}

Add-SectionHead '5. DISK-HEALTH SIGNALS'
Add-OutLine '  Recent disk/ntfs errors in System log (last 3 days, top 20):'
$since = (Get-Date).AddDays(-3)
$diskEvents = Get-WinEvent -FilterHashtable @{
        LogName   = 'System'
        StartTime = $since
    } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'disk|Ntfs|volmgr|storahci|stornvme' } |
    Select-Object -First 20
if ($diskEvents) {
    foreach ($evt in $diskEvents) {
        Add-OutLine ("    {0}  [{1}] {2}  (id {3})" -f $evt.TimeCreated, $evt.LevelDisplayName, $evt.ProviderName, $evt.Id)
    }
} else {
    Add-OutLine '    (no matching events found or log unreadable)'
}

Add-SectionHead '6. END'
Add-OutLine '  Nothing was modified (only a temp probe file was created and removed).'

$reportPath = 'G:\codex-workspace\tools\_runtime-deps.txt'
[System.IO.File]::WriteAllText($reportPath, ($script:ReportLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
$script:ReportLines | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("Report written to: " + $reportPath)

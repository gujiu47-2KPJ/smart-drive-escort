# ============================================================
#  Accurate C: space scan - does NOT follow junctions / symlinks
#
#  Why: tools that walk directories recursively (and simple
#  "measure every file" loops) FOLLOW reparse points. On this
#  machine C:\Users\gujiu\.espressif is a junction to
#  D:\Espressif and C:\Users\gujiu\.vscode is a junction to
#  D:\vscode\.vscode, so a naive scan double-counts D:'s data
#  as if it lived on C:.
#
#  This script skips reparse points entirely, so the numbers
#  reflect what is PHYSICALLY on C:.
#
#  READ-ONLY. Nothing is modified or deleted.
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\accurate-size-scan.ps1
#
#  Report: G:\codex-workspace\tools\_accurate-size-scan.txt
# ============================================================

$ErrorActionPreference = 'SilentlyContinue'
$script:ReportLines = New-Object System.Collections.Generic.List[string]
function Add-OutLine([string]$Text) { $script:ReportLines.Add([string]$Text) }
function Add-SectionHead([string]$Text) {
    Add-OutLine ''
    Add-OutLine ('=' * 72)
    Add-OutLine ('  ' + $Text)
    Add-OutLine ('=' * 72)
}

# Recursive size that skips reparse points (junctions / symlinks).
function Get-RealSize {
    param([string]$Root)

    $bytes = 0
    $skippedLinks = New-Object System.Collections.Generic.List[string]
    $stack = New-Object System.Collections.Stack
    $stack.Push($Root)

    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        $items = Get-ChildItem -LiteralPath $current -Force -ErrorAction SilentlyContinue
        foreach ($item in $items) {
            if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
                $skippedLinks.Add($item.FullName) | Out-Null
                continue
            }
            if ($item.PSIsContainer) {
                $stack.Push($item.FullName)
            } else {
                $bytes += $item.Length
            }
        }
    }

    return [pscustomobject]@{
        Bytes        = $bytes
        SkippedLinks = $skippedLinks
    }
}

Add-SectionHead '0. CONTEXT'
Add-OutLine ("  time: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

Add-SectionHead '1. DRIVE FREE SPACE'
Get-PSDrive -PSProvider FileSystem | ForEach-Object {
    if ($null -ne $_.Free -and $_.Used -gt 0) {
        Add-OutLine ("  {0}  free {1} GB / total {2} GB" -f $_.Root,
            [math]::Round($_.Free / 1GB, 2), [math]::Round(($_.Used + $_.Free) / 1GB, 2))
    }
}

Add-SectionHead '2. JUNCTIONS / SYMLINKS IN THE USER PROFILE (not real C: data)'
Get-ChildItem -LiteralPath $HOME -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint } |
    ForEach-Object {
        $target = ''
        try {
            $target = (Get-Item -LiteralPath $_.FullName -Force).Target -join ', '
        } catch { }
        Add-OutLine ("  {0,-34} -> {1}" -f $_.Name, $target)
    }

Add-SectionHead '3. KEY DIRECTORIES, MEASURED WITHOUT FOLLOWING LINKS'
foreach ($target in @(
        "$HOME\.codex",
        "$HOME\.cache",
        "$HOME\.cache\codex-runtimes",
        "$HOME\.espressif",
        "$HOME\.vscode",
        "$HOME\.omo",
        "$HOME\.reasonix",
        "$HOME\.claude",
        "$HOME\.cursor",
        "$env:APPDATA\Code",
        "$env:LOCALAPPDATA",
        "$env:APPDATA",
        "$env:LOCALAPPDATA\Temp",
        "$env:LOCALAPPDATA\npm-cache",
        "$env:APPDATA\npm"
    )) {
    if (-not (Test-Path -LiteralPath $target)) {
        Add-OutLine ("  {0,-46} (does not exist)" -f $target)
        continue
    }
    $item = Get-Item -LiteralPath $target -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        Add-OutLine ("  {0,-46} LINK -> {1}  (0 GB on C:)" -f $target, ($item.Target -join ', '))
        continue
    }
    $r = Get-RealSize -Root $target
    Add-OutLine ("  {0,-46} {1} GB" -f $target, [math]::Round($r.Bytes / 1GB, 3))
    foreach ($link in $r.SkippedLinks) {
        Add-OutLine ("      (skipped link) {0}" -f $link)
    }
}

Add-SectionHead '4. TOP CONSUMERS UNDER C: (links skipped)'
foreach ($root in @($HOME, "$env:LOCALAPPDATA", "$env:APPDATA", 'C:\ProgramData',
                    'C:\Program Files', 'C:\Program Files (x86)', 'C:\Windows')) {
    if (-not (Test-Path -LiteralPath $root)) { continue }
    Add-OutLine ''
    Add-OutLine ("  == {0} ==" -f $root)
    $rows = @()
    foreach ($dir in (Get-ChildItem -LiteralPath $root -Directory -Force -ErrorAction SilentlyContinue)) {
        if ($dir.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            $rows += [pscustomobject]@{ Path = $dir.FullName; GB = -1 }
            continue
        }
        $r = Get-RealSize -Root $dir.FullName
        $rows += [pscustomobject]@{ Path = $dir.FullName; GB = [math]::Round($r.Bytes / 1GB, 3) }
    }
    $rows | Sort-Object GB -Descending | Select-Object -First 15 | ForEach-Object {
        if ($_.GB -lt 0) {
            Add-OutLine ("  {0,-58} LINK (0 GB)" -f $_.Path)
        } else {
            Add-OutLine ("  {0,-58} {1} GB" -f $_.Path, $_.GB)
        }
    }
}

Add-SectionHead '5. END'
Add-OutLine '  Nothing was modified.'

$reportPath = 'G:\codex-workspace\tools\_accurate-size-scan.txt'
[System.IO.File]::WriteAllText($reportPath, ($script:ReportLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
$script:ReportLines | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("Report written to: " + $reportPath)

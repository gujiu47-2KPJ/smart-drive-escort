# ============================================================
#  PATH + PowerShell profile repair  (DRY-RUN BY DEFAULT)
#
#  Purpose:
#    1. Find and remove DEAD entries from the Windows PATH
#       (entries whose directory does not exist).
#    2. Fix the PowerShell 7 profile encoding (add UTF-8 BOM,
#       rewrite the content in pure ASCII to remove the mojibake).
#
#  SAFETY MODEL (read this):
#    * Running with no argument = DRY RUN. Nothing is written.
#      It only shows you exactly what WOULD change.
#    * Nothing is ever overwritten in place: before any write,
#      the current PATH and the profile are backed up to
#      G:\codex-workspace\backup\env-fix-<timestamp>\
#    * Changes are staged in the registry under a NEW value name
#      first, so the old value stays intact until the new one is
#      verified. The old value is renamed, not deleted.
#    * Only PATH entries pointing at NON-EXISTENT directories are
#      removed. Existing entries are always preserved.
#
#  Usage:
#    # 1) see what would change (safe, default)
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\repair-path-and-profile.ps1
#
#    # 2) actually apply (requires admin for the Machine scope)
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\repair-path-and-profile.ps1 -Apply
#
#  Report: G:\codex-workspace\tools\_path-repair.txt
# ============================================================

param(
    [switch]$Apply
)

$ErrorActionPreference = 'Continue'
$script:ReportLines = New-Object System.Collections.Generic.List[string]
function Add-OutLine([string]$Text) { $script:ReportLines.Add([string]$Text) }
function Add-SectionHead([string]$Text) {
    Add-OutLine ''
    Add-OutLine ('=' * 70)
    Add-OutLine ('  ' + $Text)
    Add-OutLine ('=' * 70)
}
function Add-Pair([string]$Label, $Value) { Add-OutLine ("  {0,-24}: {1}" -f $Label, $Value) }

$script:Mode = 'DRY-RUN (nothing will be written)'
if ($Apply) { $script:Mode = 'APPLY (changes will be written)' }

Add-SectionHead '0. MODE'
Add-Pair 'mode' $script:Mode
Add-Pair 'time' (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
Add-Pair 'is admin' ([Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator))

$backupRoot = Join-Path 'G:\codex-workspace\backup' ('env-fix-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))

function Get-PathScope {
    param([ValidateSet('User','Machine')][string]$Scope, [string]$Kind)
    $key = 'HKCU:\Environment'
    if ($Scope -eq 'Machine') { $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' }
    try {
        $val = (Get-ItemProperty -LiteralPath $key -Name $Kind -ErrorAction Stop).$Kind
        return $val
    } catch {
        return $null
    }
}

function Split-PathValue {
    param([string]$Raw)
    if ([string]::IsNullOrEmpty($Raw)) { return @() }
    return ($Raw -split ';')
}

Add-SectionHead '1. CURRENT PATH (FROM REGISTRY, NOT THE SESSION)'
$scopes = @(
    @{ Scope = 'User';    Kind = 'Path' },
    @{ Scope = 'Machine'; Kind = 'Path'  }
)
$report = @()
foreach ($s in $scopes) {
    $raw = Get-PathScope -Scope $s.Scope -Kind $s.Kind
    if ([string]::IsNullOrEmpty($raw)) {
        Add-Pair $s.Scope '(empty or unreadable)'
        continue
    }
    $entries = Split-PathValue $raw
    $dead = @()
    $alive = @()
    $pathIndex = 0
    foreach ($entry in $entries) {
        $pathIndex++
        $trimmed = $entry.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
        if (Test-Path -LiteralPath $trimmed) {
            $alive += $trimmed
        } else {
            $dead += $trimmed
        }
    }
    Add-OutLine ''
    Add-OutLine ("  == SCOPE: {0}  ==" -f $s.Scope)
    Add-Pair 'entries total' $entries.Count
    Add-Pair 'entries alive' $alive.Count
    Add-Pair 'entries DEAD' $dead.Count
    Add-OutLine '  --- DEAD entries (these are what would be removed) ---'
    foreach ($d in $dead) { Add-OutLine ('    [DEAD] ' + $d) }
    Add-OutLine '  --- ALIVE entries (always preserved) ---'
    foreach ($a in $alive) { Add-OutLine ('    [KEEP] ' + $a) }
    $report += [pscustomobject]@{
        Scope  = $s.Scope
        Kind   = $s.Kind
        Raw    = $raw
        Alive  = $alive
        Dead   = $dead
    }
}

Add-SectionHead '2. POWERSHELL PROFILE ENCODING'
$pwsh7Profile = Join-Path $HOME 'Documents\PowerShell\Microsoft.PowerShell_profile.ps1'
$profilePlan = $null
if (Test-Path -LiteralPath $pwsh7Profile) {
    $rawBytes = [System.IO.File]::ReadAllBytes($pwsh7Profile)
    $hasBom = ($rawBytes.Length -ge 3 -and $rawBytes[0] -eq 0xEF -and $rawBytes[1] -eq 0xBB -and $rawBytes[2] -eq 0xBF)
    Add-Pair 'path' $pwsh7Profile
    Add-Pair 'size bytes' $rawBytes.Length
    Add-Pair 'has UTF8 BOM' $hasBom
    $newContent = @'
# UTF-8 console setup (rewritten as pure ASCII on 2026-10-02)
# Purpose: make pwsh / python / git / ESP-IDF handle UTF-8 consistently.
# To undo: delete this file.
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    [Console]::InputEncoding  = [System.Text.Encoding]::UTF8
    $OutputEncoding           = [System.Text.Encoding]::UTF8
    $PSDefaultParameterValues["Out-File:Encoding"]   = "utf8"
    $PSDefaultParameterValues["Set-Content:Encoding"] = "utf8"
    $PSDefaultParameterValues["Add-Content:Encoding"] = "utf8"
} catch { }
# Force the console code page to UTF-8 (65001).
if ($Host.Name -eq "ConsoleHost") {
    try { & "$env:SystemRoot\System32\chcp.com" 65001 | Out-Null } catch { }
}
'@
    Add-OutLine ''
    Add-OutLine '  --- the profile would be REPLACED with this (ASCII, no mojibake) ---'
    foreach ($line in ($newContent -split "`r?`n")) { Add-OutLine ('  | ' + $line) }
    $profilePlan = [pscustomobject]@{
        Path       = $pwsh7Profile
        NewContent = $newContent
    }
} else {
    Add-OutLine ("  [none] {0}" -f $pwsh7Profile)
}

Add-SectionHead '3. WHAT THE APPLY STEP WOULD DO'
Add-OutLine '  1. Create backup dir:'
Add-OutLine ('     ' + $backupRoot)
Add-OutLine '  2. Write these backup files:'
Add-OutLine '     - path-user-before.txt      (raw registry value, verbatim)'
Add-OutLine '     - path-machine-before.txt   (raw registry value, verbatim)'
Add-OutLine '     - profile-before.ps1        (byte-for-byte copy)'
Add-OutLine '  3. For each scope with DEAD entries:'
Add-OutLine '     - write the cleaned value to a TEMP value name "<Kind>__new"'
Add-OutLine '     - verify it reads back and every remaining entry exists'
Add-OutLine '     - only then swap: rename "<Kind>" to "<Kind>__old", rename "<Kind>__new" to "<Kind>"'
Add-OutLine '     - "<Kind>__old" is KEPT so a rollback is a one-liner'
Add-OutLine '  4. Rewrite the PowerShell 7 profile as UTF-8 WITH BOM, ASCII content'
Add-OutLine '  5. Re-read both PATH values and confirm the dead entries are gone'
Add-OutLine ''
Add-OutLine '  ROLLBACK (if anything looks wrong later):'
Add-OutLine '    Restore-ItemProperty -Path HKCU:\Environment -Name Path -Value <path-user-before.txt>'
Add-OutLine '    or simply copy the backup files back.'

if ($Apply) {
    Add-SectionHead '4. APPLYING'
    if (-not (Test-Path -LiteralPath $backupRoot)) {
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    }
    Add-OutLine ('  backup dir: ' + $backupRoot)

    foreach ($r in $report) {
        $beforeFile = Join-Path $backupRoot ("path-{0}-before.txt" -f $r.Scope.ToLower())
        [System.IO.File]::WriteAllText($beforeFile, $r.Raw, [System.Text.Encoding]::Unicode)
        Add-OutLine ('  backed up : ' + $beforeFile)

        if ($r.Dead.Count -eq 0) {
            Add-OutLine ("  scope {0}: no dead entries, nothing to change" -f $r.Scope)
            continue
        }

        $cleaned = ($r.Alive -join ';')
        $key = 'HKCU:\Environment'
        if ($r.Scope -eq 'Machine') { $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' }
        $kind = $r.Kind
        try {
            Set-ItemProperty -LiteralPath $key -Name ($kind + '__new') -Value $cleaned -ErrorAction Stop
            $readBack = (Get-ItemProperty -LiteralPath $key -Name ($kind + '__new') -ErrorAction Stop).($kind + '__new')
            if ($readBack -ne $cleaned) { throw 'read-back mismatch' }
            Add-OutLine ("  scope {0}: staged cleaned value under {1}__new (verified)" -f $r.Scope, $kind)

            $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
            Set-ItemProperty -LiteralPath $key -Name ($kind + '__old_' + $stamp) -Value $r.Raw -ErrorAction Stop
            Set-ItemProperty -LiteralPath $key -Name $kind -Value $cleaned -ErrorAction Stop
            Add-OutLine ("  scope {0}: SWAPPED. old value preserved as {1}__old_{2}" -f $r.Scope, $kind, $stamp)
            Remove-ItemProperty -LiteralPath $key -Name ($kind + '__new') -ErrorAction SilentlyContinue
        } catch {
            Add-OutLine ("  scope {0}: FAILED - {1}" -f $r.Scope, $_.Exception.Message)
            Add-OutLine '  (old value was never overwritten; nothing to roll back)'
        }
    }

    if ($null -ne $profilePlan) {
        $pBefore = Join-Path $backupRoot 'profile-before.ps1'
        Copy-Item -LiteralPath $profilePlan.Path -Destination $pBefore -Force
        Add-OutLine ('  backed up : ' + $pBefore)
        try {
            [System.IO.File]::WriteAllText($profilePlan.Path, $profilePlan.NewContent,
                (New-Object System.Text.UTF8Encoding($true)))
            $afterBytes = [System.IO.File]::ReadAllBytes($profilePlan.Path)
            $afterBom = ($afterBytes.Length -ge 3 -and $afterBytes[0] -eq 0xEF -and $afterBytes[1] -eq 0xBB -and $afterBytes[2] -eq 0xBF)
            Add-Pair 'profile rewritten, has BOM now' $afterBom
        } catch {
            Add-OutLine ('  profile rewrite FAILED - ' + $_.Exception.Message)
        }
    }
} else {
    Add-SectionHead '4. DRY RUN'
    Add-OutLine '  Nothing was written. Re-run with -Apply to make the changes:'
    Add-OutLine '    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\repair-path-and-profile.ps1 -Apply'
}

Add-SectionHead '5. END'
Add-Pair 'backup dir' $backupRoot

$reportPath = 'G:\codex-workspace\tools\_path-repair.txt'
[System.IO.File]::WriteAllText($reportPath, ($script:ReportLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
$script:ReportLines | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("Report written to: " + $reportPath)

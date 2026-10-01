# ============================================================
#  Fix the Codex Windows sandbox setup failure
#
#  SYMPTOM
#    Every non-elevated (sandboxed) command in Codex fails with:
#      CreateProcess ... helper_unknown_error: setup refresh had errors
#    Only escalated commands run.
#
#  ROOT CAUSE  (evidence from C:\Users\gujiu\.codex\.sandbox\*.log)
#    The sandbox setup protects paths inside a writable root by writing a
#    deny ACE onto them (.git, .agents, .codex, .aws). That needs WRITE_DAC,
#    which only the object OWNER or an account holding FullControl in an
#    ELEVATED token has.
#
#      deny ACE failed on G:\codex-workspace\.git: open deny ACL target for update
#      setup refresh completed with errors
#
#    G:\codex-workspace\.git is owned by "TIANXUAN\CodexSandboxOffline"
#    because it was created by git running INSIDE the sandbox on 2026-10-01
#    14:13:37. The sandbox logs show:
#      sandbox.2026-06-06.log : no deny-ACE failure (setup completed)
#      sandbox.2026-09-30.log : no deny-ACE failure (setup completed)
#      sandbox.2026-10-01.log : FIRST deny-ACE failure at line 950
#      sandbox.2026-10-02.log : fails on every attempt
#    i.e. the sandbox broke exactly when .git was created.
#
#  FIX
#    Take ownership of .git back for the real user, and grant that user
#    FullControl on it. The ACL is backed up first.
#
#  SAFETY
#    - only touches G:\codex-workspace\.git
#    - never touches git history, the working tree, or any other path
#    - prints BEFORE and AFTER owner/ACL so the change is visible
#    - backs up the ACL; rollback command is printed at the end
#    - needs one administrator approval (UAC); it asks for that itself
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\fix-sandbox-git-acl.ps1
#
#  Report: G:\codex-workspace\tools\_sandbox-acl-fix.txt
# ============================================================

param(
    [switch]$Elevated
)

$script:ReportLines = New-Object System.Collections.Generic.List[string]
function Say([string]$Text) {
    $script:ReportLines.Add([string]$Text)
    Write-Host $Text
}

$reportPath = 'G:\codex-workspace\tools\_sandbox-acl-fix.txt'

$isAdmin = ([Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator))

# ---------- step 0: self-elevate ----------
if (-not $isAdmin -and -not $Elevated) {
    Say 'Not elevated. Relaunching with administrator rights...'
    Say 'Please approve the UAC prompt.'
    try {
        $childArgs = @(
            '-NoProfile',
            '-ExecutionPolicy', 'Bypass',
            '-File', $PSCommandPath,
            '-Elevated'
        )
        $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $childArgs -Verb RunAs -Wait -PassThru
        Say ("Elevated run finished with exit code: {0}" -f $proc.ExitCode)
    } catch {
        Say ("Could not relaunch elevated: {0}" -f $_.Exception.Message)
        Say 'Right-click PowerShell, choose "Run as administrator", then run:'
        Say ("  powershell -NoProfile -ExecutionPolicy Bypass -File {0}" -f $PSCommandPath)
    }
    if (Test-Path -LiteralPath $reportPath) {
        Say ''
        Say '=== report from the elevated run ==='
        Get-Content -LiteralPath $reportPath | ForEach-Object { Write-Host $_ }
    }
    exit 0
}

# ---------- main work (elevated) ----------
$gitPath = 'G:\codex-workspace\.git'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = "G:\codex-workspace\backup\git-acl-$stamp"

Say ('=' * 66)
Say '  Codex sandbox ACL repair'
Say ('=' * 66)
Say ("  time      : {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Say ("  elevated  : {0}" -f $isAdmin)
Say ("  identity  : {0}\{1}" -f $env:USERDOMAIN, $env:USERNAME)
Say ("  target    : {0}" -f $gitPath)

if (-not (Test-Path -LiteralPath $gitPath)) {
    Say ("  FATAL: .git not found at {0}" -f $gitPath)
    [System.IO.File]::WriteAllText($reportPath, ($script:ReportLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
    exit 1
}

Say ''
Say '--- 1. BEFORE ---'
$beforeAcl = Get-Acl -LiteralPath $gitPath
Say ("  owner : {0}" -f $beforeAcl.Owner)
foreach ($ace in $beforeAcl.Access) {
    Say ("    {0,-46} {1,-6} {2}" -f $ace.IdentityReference, $ace.AccessControlType, $ace.FileSystemRights)
}

Say ''
Say '--- 2. backup ACL ---'
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
$aclBackupFile = Join-Path $backupDir 'git-acl.txt'
& icacls $gitPath /save $aclBackupFile /T /C 2>&1 | ForEach-Object { Say ('  ' + $_) }
Say ("  saved: {0}" -f $aclBackupFile)

Say ''
Say '--- 3. takeown ---'
& takeown /f $gitPath /r /d Y 2>&1 | ForEach-Object { Say ('  ' + $_) }

Say ''
Say '--- 4. grant FullControl to current user ---'
$grantTarget = "$env:USERDOMAIN\$env:USERNAME"
& icacls $gitPath /grant "${grantTarget}:(OI)(CI)F" /T /C 2>&1 | ForEach-Object { Say ('  ' + $_) }

Say ''
Say '--- 5. AFTER ---'
$afterAcl = Get-Acl -LiteralPath $gitPath
Say ("  owner : {0}" -f $afterAcl.Owner)
foreach ($ace in $afterAcl.Access) {
    Say ("    {0,-46} {1,-6} {2}" -f $ace.IdentityReference, $ace.AccessControlType, $ace.FileSystemRights)
}

Say ''
Say '--- 6. WRITE_DAC verification (no-op Set-Acl) ---'
$writeDacOk = $false
try {
    $acl = Get-Acl -LiteralPath $gitPath
    Set-Acl -LiteralPath $gitPath -AclObject $acl -ErrorAction Stop
    $writeDacOk = $true
} catch {
    Say ("  Set-Acl failed: {0}" -f $_.Exception.Message)
}
Say ("  public user can write the DACL now : {0}" -f $writeDacOk)

Say ''
Say '--- 7. git still healthy? ---'
$gitExe = 'G:\Git\cmd\git.exe'
if (Test-Path -LiteralPath $gitExe) {
    & $gitExe -C 'G:\codex-workspace' status --short --branch 2>&1 | Select-Object -First 6 | ForEach-Object { Say ('  ' + $_) }
} else {
    Say '  (git.exe not found at G:\Git\cmd - skipped)'
}

Say ''
Say '--- 8. summary ---'
Say ("  backup : {0}" -f $aclBackupFile)
Say '  rollback:'
Say ("    icacls G:\codex-workspace /restore `"{0}`"" -f $aclBackupFile)
Say ''
Say '  Next: run any command in Codex. Sandboxed exec should now start.'

[System.IO.File]::WriteAllText($reportPath, ($script:ReportLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
Say ''
Say ("  report written to: {0}" -f $reportPath)

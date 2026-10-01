# ============================================================
#  READ-ONLY: dig out MCP server definitions configured for
#  Reasonix / OpenCode / other agents on this machine.
#
#  Output is SHORT and designed to be copy-pasted.
#  Only searches a bounded set of directories (no full C: walk).
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File G:\codex-workspace\tools\dig-mcp.ps1
#
#  Report: G:\codex-workspace\tools\_mcp-dig.txt
# ============================================================

$ErrorActionPreference = 'SilentlyContinue'
$script:ReportLines = New-Object System.Collections.Generic.List[string]
function Add-OutLine([string]$Text) { $script:ReportLines.Add([string]$Text) }
function Add-SectionHead([string]$Text) {
    Add-OutLine ''
    Add-OutLine ('--- ' + $Text + ' ---')
}

$searchRoots = @(
    'G:\reasonix_workspace',
    'G:\.reasonix',
    'D:\opencode',
    "$HOME\.config\opencode",
    "$HOME\.opencode",
    "$HOME\.codex",
    "$HOME\.claude",
    'G:\temp_doc',
    'G:\temp_amct'
)

Add-SectionHead 'SEARCH ROOTS'
foreach ($r in $searchRoots) {
    Add-OutLine ("  {0}  exists={1}" -f $r, (Test-Path -LiteralPath $r))
}

Add-SectionHead 'FILES NAMED LIKE CONFIG IN THOSE ROOTS (depth 3)'
$candidates = @()
foreach ($r in $searchRoots) {
    if (-not (Test-Path -LiteralPath $r)) { continue }
    $candidates += Get-ChildItem -LiteralPath $r -Recurse -Depth 3 -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(mcp|opencode|config|settings|agents|servers|.*config.*)\.(json|jsonc|toml|ya?ml)$' }
}
$candidates = $candidates | Sort-Object FullName -Unique
foreach ($c in $candidates) {
    Add-OutLine ("  {0}   ({1} bytes)" -f $c.FullName, $c.Length)
}

Add-SectionHead 'MCP-ISH CONTENT HITS (grep for "mcp")'
foreach ($c in $candidates) {
    $hits = Select-String -LiteralPath $c.FullName -Pattern 'mcp' -SimpleMatch -ErrorAction SilentlyContinue
    if ($hits) {
        Add-OutLine ''
        Add-OutLine ("  FILE: {0}" -f $c.FullName)
        $hits | Select-Object -First 40 | ForEach-Object {
            Add-OutLine ("    L{0}: {1}" -f $_.LineNumber, $_.Line.Trim())
        }
    }
}

Add-SectionHead 'KNOWN GLOBAL MCP PACKAGES'
$npmRoot = "$env:APPDATA\npm\node_modules"
foreach ($pkgName in @('@modelcontextprotocol', '@upstash', '@wonderwhy-er', 'context7')) {
    $pkgDir = Join-Path $npmRoot $pkgName
    Add-OutLine ''
    Add-OutLine ("  PACKAGE: {0}" -f $pkgName)
    if (-not (Test-Path -LiteralPath $pkgDir)) {
        Add-OutLine '    (not found)'
        continue
    }
    # scoped packages: enumerate one level deeper
    if ($pkgName.StartsWith('@')) {
        Get-ChildItem -LiteralPath $pkgDir -Directory -Force -ErrorAction SilentlyContinue | ForEach-Object {
            Add-OutLine ("    SUB: {0}" -f $_.Name)
            $pj = Join-Path $_.FullName 'package.json'
            if (Test-Path -LiteralPath $pj) {
                try {
                    $pkg = Get-Content -LiteralPath $pj -Raw | ConvertFrom-Json
                    Add-OutLine ("      version     : {0}" -f $pkg.version)
                    Add-OutLine ("      description : {0}" -f $pkg.description)
                    if ($pkg.bin) {
                        foreach ($bp in $pkg.bin.PSObject.Properties) {
                            Add-OutLine ("      bin         : {0} -> {1}" -f $bp.Name, $bp.Value)
                        }
                    }
                } catch { }
            }
        }
    } else {
        $pj = Join-Path $pkgDir 'package.json'
        if (Test-Path -LiteralPath $pj) {
            try {
                $pkg = Get-Content -LiteralPath $pj -Raw | ConvertFrom-Json
                Add-OutLine ("    version     : {0}" -f $pkg.version)
                Add-OutLine ("    description : {0}" -f $pkg.description)
                if ($pkg.bin) {
                    foreach ($bp in $pkg.bin.PSObject.Properties) {
                        Add-OutLine ("    bin         : {0} -> {1}" -f $bp.Name, $bp.Value)
                    }
                }
            } catch { }
        }
    }
}

Add-SectionHead 'NPM SHIMS INSTALLED'
Get-ChildItem -LiteralPath "$env:APPDATA\npm" -File -Force -ErrorAction SilentlyContinue |
    ForEach-Object { Add-OutLine ("  {0}" -f $_.Name) }

Add-SectionHead 'CODEX CONFIG CONTENT'
$codexConfig = "$HOME\.codex\config.toml"
if (Test-Path -LiteralPath $codexConfig) {
    Get-Content -LiteralPath $codexConfig -ErrorAction SilentlyContinue | ForEach-Object { Add-OutLine ('  | ' + $_) }
} else {
    Add-OutLine '  (missing)'
}

Add-SectionHead 'CLAUDE CONFIGS'
foreach ($cf in @("$HOME\.claude.json", "$HOME\.claude\settings.json")) {
    Add-OutLine ''
    Add-OutLine ("  FILE: {0}" -f $cf)
    if (Test-Path -LiteralPath $cf) {
        Get-Content -LiteralPath $cf -ErrorAction SilentlyContinue | ForEach-Object { Add-OutLine ('  | ' + $_) }
    } else {
        Add-OutLine '  (missing)'
    }
}

Add-SectionHead 'SERIAL MCP TRACES (evidence of a serial MCP server)'
$traceFile = 'G:\.serial_mcp\traces\trace.jsonl'
if (Test-Path -LiteralPath $traceFile) {
    Add-OutLine ("  size: {0} bytes" -f (Get-Item -LiteralPath $traceFile).Length)
    Get-Content -LiteralPath $traceFile -Tail 3 -ErrorAction SilentlyContinue | ForEach-Object {
        $line = $_
        if ($line.Length -gt 400) { $line = $line.Substring(0, 400) + '...' }
        Add-OutLine ('  | ' + $line)
    }
} else {
    Add-OutLine '  (missing)'
}

Add-SectionHead 'END'

$reportPath = 'G:\codex-workspace\tools\_mcp-dig.txt'
[System.IO.File]::WriteAllText($reportPath, ($script:ReportLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
$script:ReportLines | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("Report written to: " + $reportPath)

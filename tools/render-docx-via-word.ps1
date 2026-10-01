<#
  用本机 Microsoft Word 把 DOCX 渲染成逐页 PNG（供排版检查）。

  为什么不用技能自带的 render_docx.py：
    它依赖 LibreOffice(soffice)，这台机器上没装；而 Word 是装着的。
    渲染目的（逐页目视检查排版）不变，只是换了渲染后端。

  链路：DOCX --Word--> PDF --poppler(pdftoppm)--> page-N.png

  用法：
    pwsh -File render-docx-via-word.ps1 -Docx a.docx -OutDir out [-Dpi 150]

  注意：必须在真实用户会话中运行（沙箱内 Word COM 起不来）。
#>
param(
    [Parameter(Mandatory = $true)][string]$Docx,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Dpi = 150
)

$ErrorActionPreference = 'Stop'
$Docx = [IO.Path]::GetFullPath($Docx)
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (-not (Test-Path -LiteralPath $Docx)) { throw "找不到输入文件: $Docx" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$pdf = Join-Path $OutDir ([IO.Path]::GetFileNameWithoutExtension($Docx) + '.pdf')

$word = $null
$doc = $null
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $doc = $word.Documents.Open($Docx, $false, $true)
    try { $doc.Fields.Update() | Out-Null } catch { }
    $doc.ExportAsFixedFormat($pdf, 17)
    Write-Host "PDF 输出: $pdf"
}
finally {
    if ($doc) { try { $doc.Close($false) } catch { } }
    if ($word) { try { $word.Quit() } catch { } }
    try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($word) } catch { }
    [GC]::Collect()
}

$poppler = 'C:\Users\gujiu\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\poppler\Library\bin\pdftoppm.exe'
if (-not (Test-Path -LiteralPath $poppler)) { throw "找不到 pdftoppm: $poppler" }

Get-ChildItem -LiteralPath $OutDir -Filter 'page-*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
$prefix = Join-Path $OutDir 'page'
& $poppler -png -r $Dpi $pdf $prefix

$pages = Get-ChildItem -LiteralPath $OutDir -Filter 'page-*.png' | Sort-Object Name
Write-Host ("渲染完成: {0} 页" -f $pages.Count)
$pages | ForEach-Object { Write-Host ("  {0}  {1:N0} KB" -f $_.Name, ($_.Length / 1KB)) }

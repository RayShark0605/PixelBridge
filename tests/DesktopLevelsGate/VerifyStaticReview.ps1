#Requires -Version 7.2
param([Parameter(Mandatory)][string]$ScratchRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'StaticReview.ps1')
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$reviewPath = Join-Path $PSScriptRoot 'cppcheck_review.json'
$review = Get-Content -LiteralPath $reviewPath -Raw | ConvertFrom-Json
$directory = Join-Path $ScratchRoot ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($directory) | Out-Null
$reportPath = Join-Path $directory 'report.xml'
$item = $review.Entries[0]
$message = [Security.SecurityElement]::Escape($item.Message)
$valid = "<results><cppcheck version='$($review.Cppcheck)'/><errors><error id='$($item.Id)' severity='$($item.Severity)' msg='$message'><location file='$($item.File)' line='$($item.Line)'/></error></errors></results>"
[IO.File]::WriteAllText($reportPath, $valid)
$result = Get-CppcheckReview $reportPath 2 $source $reviewPath
if ($result.Findings -ne 1) { throw 'Known exact reviewed diagnostic was not verified' }
$absolute = [Security.SecurityElement]::Escape((Join-Path $source $item.File).Replace('\','/'))
[IO.File]::WriteAllText($reportPath, $valid.Replace("file='$($item.File)'", "file='$absolute'"))
$result = Get-CppcheckReview $reportPath 2 $source $reviewPath
if ($result.Findings -ne 1) { throw 'Absolute diagnostic path did not resolve to the same reviewed source' }
$empty = "<results><cppcheck version='$($review.Cppcheck)'/><errors/></results>"
[IO.File]::WriteAllText($reportPath, $empty)
$result = Get-CppcheckReview $reportPath 0 $source $reviewPath
if ($result.Findings -ne 0) { throw 'Successful zero-diagnostic cppcheck XML was not verified' }
foreach ($bad in @($valid.Replace("id='$($item.Id)'", "id='newFinding'"),
    $valid.Replace("severity='$($item.Severity)'", "severity='error'"),
    $valid.Replace("line='$($item.Line)'", "line='9999'"),
    $valid.Replace("version='$($review.Cppcheck)'", "version='unknown'"),
    $valid.Replace("file='$($item.File)'", "file='../outside-review.cpp'"),
    "<results><cppcheck version='$($review.Cppcheck)'/><errors/></results>"))
{
    [IO.File]::WriteAllText($reportPath, $bad)
    $rejected = $false
    try { $null = Get-CppcheckReview $reportPath 2 $source $reviewPath }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Unknown/changed static diagnostic was accepted' }
}
foreach ($missing in @("<results><cppcheck version='$($review.Cppcheck)'/></results>", '<results><errors/></results>'))
{
    [IO.File]::WriteAllText($reportPath, $missing)
    $rejected = $false
    try { $null = Get-CppcheckReview $reportPath 0 $source $reviewPath }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Incomplete successful XML report was accepted' }
}
[IO.File]::WriteAllText($reportPath, $valid)
$review.Entries[0].SourceSHA256 = '0' * 64
$changedReview = Join-Path $directory 'changed-review.json'
$review | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $changedReview
$rejected = $false
try { $null = Get-CppcheckReview $reportPath 2 $source $changedReview }
catch { $rejected = $true }
if (-not $rejected) { throw 'Mismatched source hash was accepted' }
Write-Output 'DESKTOP_LEVELS_STATIC_REVIEW_VALIDATOR_PASS known=2 empty=1 rejected=9'

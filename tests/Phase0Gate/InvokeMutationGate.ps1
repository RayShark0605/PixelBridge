[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FuzzBinaryDirectory,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$FuzzBinaryDirectory = (Resolve-Path -LiteralPath $FuzzBinaryDirectory).Path
$EvidenceDirectory = [System.IO.Path]::GetFullPath($EvidenceDirectory)
[System.IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null
$summaryPath = Join-Path $EvidenceDirectory 'mutation-results.jsonl'
Set-Content -LiteralPath $summaryPath -Value '' -NoNewline -Encoding utf8

# Decimal strings avoid PowerShell's signed-integer/JSON precision ambiguity.
$runs = @(
    @('PBProtocolDescriptorResourceFuzz', '100000', '13464654573299691533'),
    @('PBProtocolBootstrapControlFuzz', '100000', '5783258900934164481'),
    @('PBProtocolOrphanResourceFuzz', '100000', '7263948150273648113'),
    @('PBCompressionZstdBoundaryFuzz', '100000', '13856851484949778996'),
    @('PBOuterFecWirehairV2Fuzz', '100000', '6289371488644456784'),
    @('PBOuterFecDirectRepeatFuzz', '100000', '4923072552113298010'),
    @('PBProtocolTransportFuzz', '100000', '14627333968688430831'),
    @('PBProtocolResumeStateFuzz', '100000', '14059238575124963127'),
    @('PBModulationReferenceRasterFuzz', '10000', '4761939155204273851'),
    @('PBInterleaveReferenceFuzz', '10000', '1161981756646125696'),
    @('PBInnerFecCodewordFuzz', '10000', '11068027678940948070')
)

foreach ($run in $runs)
{
    $executable = Join-Path $FuzzBinaryDirectory ($run[0] + '.exe')
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf))
    {
        throw "Missing built driver: $executable"
    }
    $logPath = Join-Path $EvidenceDirectory ($run[0] + '.log')
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $lines = @(& $executable $run[1] $run[2] 2>&1)
    $exitCode = $LASTEXITCODE
    $stopwatch.Stop()
    $output = ($lines | ForEach-Object { $_.ToString() }) -join "`n"
    Set-Content -LiteralPath $logPath -Value $output -Encoding utf8
    $expectedMarker = 'FUZZ_COMPLETED iterations=' + $run[1] + ' seed=' + $run[2]
    $hasMarker = $output.Contains($expectedMarker)
    $sanitizerDiagnostic = $output -match 'AddressSanitizer:|ERROR:.*Sanitizer|SUMMARY:.*Sanitizer|runtime error:'
    $passed = $exitCode -eq 0 -and $hasMarker -and -not $sanitizerDiagnostic
    $result = [ordered]@{
        driver = $run[0]
        status = $(if ($passed) { 'pass' } else { 'fail' })
        iterations = [int]$run[1]
        seed = $run[2]
        exit_code = $exitCode
        completion_marker = $hasMarker
        sanitizer_diagnostic = $sanitizerDiagnostic
        elapsed_ms = $stopwatch.ElapsedMilliseconds
        mode = 'MSVC deterministic mutation + ASan; not libFuzzer or UBSan'
    }
    $json = $result | ConvertTo-Json -Compress
    Add-Content -LiteralPath $summaryPath -Value $json -Encoding utf8
    Write-Output $json
    if (-not $passed)
    {
        throw "Mutation gate failed; preserve and inspect $logPath"
    }
}

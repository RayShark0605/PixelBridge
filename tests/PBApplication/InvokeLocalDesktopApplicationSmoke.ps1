#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../..'),
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][string]$EvidenceRoot,
    [Parameter(Mandatory)][int]$MonitorOriginX,
    [Parameter(Mandatory)][int]$MonitorOriginY,
    [string[]]$CaseName = @(),
    [ValidateRange(5,120)][int]$EncoderBroadcastSeconds = 30,
    [ValidateRange(10,180)][int]$DecoderTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$BuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
if (Test-Path -LiteralPath $EvidenceRoot)
{
    throw "EvidenceRoot must be a new path: $EvidenceRoot"
}
[IO.Directory]::CreateDirectory($EvidenceRoot) | Out-Null
$roiRight64 = [int64]$MonitorOriginX + 1920
$roiBottom64 = [int64]$MonitorOriginY + 1080
if ($roiRight64 -gt [int]::MaxValue -or $roiRight64 -lt [int]::MinValue -or
    $roiBottom64 -gt [int]::MaxValue -or $roiBottom64 -lt [int]::MinValue)
{
    throw 'Monitor origin overflows the Phase-1 ROI coordinates'
}
$roiRight = [int]$roiRight64
$roiBottom = [int]$roiBottom64

$encoder = Join-Path $BuildRoot 'apps/PixelBridgeEncoder/Release/PixelBridgeEncoder.exe'
$decoder = Join-Path $BuildRoot 'apps/PixelBridgeDecoder/Release/PixelBridgeDecoder.exe'
foreach ($executable in @($encoder, $decoder))
{
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf))
    {
        throw "Missing formal application executable: $executable"
    }
}

function Write-RandomFile([string]$Path, [int]$Bytes)
{
    $buffer = [byte[]]::new($Bytes)
    [Security.Cryptography.RandomNumberGenerator]::Fill($buffer)
    [IO.File]::WriteAllBytes($Path, $buffer)
}

function Wait-ProcessBounded([Diagnostics.Process]$Process, [int]$Seconds, [string]$Role)
{
    if (-not $Process.WaitForExit($Seconds * 1000))
    {
        $Process.Kill($true)
        $Process.WaitForExit()
        throw "$Role exceeded its outer process deadline"
    }
    $Process.Refresh()
}

$sources = Join-Path $EvidenceRoot 'sources'
[IO.Directory]::CreateDirectory($sources) | Out-Null
$random1MiB = Join-Path $sources 'random-1MiB.bin'
$random8MiB = Join-Path $sources 'random-8MiB.bin'
$compressible1MiB = Join-Path $sources 'compressible-1MiB.bin'
Write-RandomFile $random1MiB (1MB)
Write-RandomFile $random8MiB (8MB)
$compressibleBuffer = [byte[]]::new(1MB)
[Array]::Fill[byte]($compressibleBuffer, [byte][char]'A')
[IO.File]::WriteAllBytes($compressible1MiB, $compressibleBuffer)

$archiveInput = Join-Path $sources 'archive-input'
[IO.Directory]::CreateDirectory($archiveInput) | Out-Null
Copy-Item -LiteralPath $random1MiB -Destination (Join-Path $archiveInput 'payload.bin')
$archive = Join-Path $sources 'already-compressed.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($archiveInput, $archive,
    [IO.Compression.CompressionLevel]::Optimal, $false)

$cases = @(
    [pscustomobject]@{ Name='random-1MiB-off-wgc-direct'; Source=$random1MiB; Compression='off'; Backend='wgc'; Profile='direct'; ExpectedCodec=$null },
    [pscustomobject]@{ Name='random-1MiB-on-dxgi-direct'; Source=$random1MiB; Compression='on'; Backend='dxgi'; Profile='direct'; ExpectedCodec=$null },
    [pscustomobject]@{ Name='random-8MiB-off-wgc-direct'; Source=$random8MiB; Compression='off'; Backend='wgc'; Profile='direct'; ExpectedCodec=$null },
    [pscustomobject]@{ Name='random-8MiB-on-dxgi-direct'; Source=$random8MiB; Compression='on'; Backend='dxgi'; Profile='direct'; ExpectedCodec=$null },
    [pscustomobject]@{ Name='zip-off-wgc-direct'; Source=$archive; Compression='off'; Backend='wgc'; Profile='direct'; ExpectedCodec='RAW' },
    [pscustomobject]@{ Name='zip-on-dxgi-direct'; Source=$archive; Compression='on'; Backend='dxgi'; Profile='direct'; ExpectedCodec='RAW' },
    [pscustomobject]@{ Name='compressible-on-wgc-direct'; Source=$compressible1MiB; Compression='on'; Backend='wgc'; Profile='direct'; ExpectedCodec='zstd' },
    [pscustomobject]@{ Name='random-1MiB-off-dxgi-shape'; Source=$random1MiB; Compression='off'; Backend='dxgi'; Profile='shape'; ExpectedCodec='RAW' }
)
if ($CaseName.Count -ne 0)
{
    $selectedNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $CaseName)
    {
        if (-not $selectedNames.Add($name))
        {
            throw "Duplicate CaseName: $name"
        }
    }
    $cases = @($cases | Where-Object { $selectedNames.Contains($_.Name) })
    if ($cases.Count -ne $selectedNames.Count)
    {
        $knownNames = @($cases.Name) -join ', '
        throw "One or more CaseName values are unknown; selected known cases: $knownNames"
    }
}

$results = [Collections.Generic.List[object]]::new()
foreach ($case in $cases)
{
    Write-Host "PB_APPLICATION_SMOKE_CASE_START name=$($case.Name)"
    $caseRoot = Join-Path $EvidenceRoot $case.Name
    $outputRoot = Join-Path $caseRoot 'output'
    [IO.Directory]::CreateDirectory($outputRoot) | Out-Null

    $encoderReport = Join-Path $caseRoot 'encoder-report.json'
    $decoderReport = Join-Path $caseRoot 'decoder-report.json'
    $encoderStdout = Join-Path $caseRoot 'encoder.stdout.log'
    $encoderStderr = Join-Path $caseRoot 'encoder.stderr.log'
    $decoderStdout = Join-Path $caseRoot 'decoder.stdout.log'
    $decoderStderr = Join-Path $caseRoot 'decoder.stderr.log'
    $encoderArguments = '--headless-broadcast --source "{0}" --profile {1} --compression {2} --origin {3} {4} --seconds {5} --report "{6}"' -f
        $case.Source,$case.Profile,$case.Compression,$MonitorOriginX,$MonitorOriginY,$EncoderBroadcastSeconds,$encoderReport
    $encoderProcess = Start-Process -FilePath $encoder -ArgumentList $encoderArguments `
        -WorkingDirectory (Split-Path -Parent $encoder) -WindowStyle Hidden `
        -RedirectStandardOutput $encoderStdout -RedirectStandardError $encoderStderr -PassThru
    $decoderProcess = $null
    $encoderAliveWhenDecoderCompleted = $false
    try
    {
        Start-Sleep -Seconds 4
        $decoderArguments = '--headless-receive --output-dir "{0}" --backend {1} --profile {2} --roi {3} {4} {5} {6} --timeout {7} --report "{8}"' -f
            $outputRoot,$case.Backend,$case.Profile,$MonitorOriginX,$MonitorOriginY,$roiRight,$roiBottom,$DecoderTimeoutSeconds,$decoderReport
        $decoderProcess = Start-Process -FilePath $decoder -ArgumentList $decoderArguments `
            -WorkingDirectory (Split-Path -Parent $decoder) -WindowStyle Hidden `
            -RedirectStandardOutput $decoderStdout -RedirectStandardError $decoderStderr -PassThru
        Wait-ProcessBounded $decoderProcess ($DecoderTimeoutSeconds + 15) 'Decoder'
        $encoderProcess.Refresh()
        $encoderAliveWhenDecoderCompleted = -not $encoderProcess.HasExited
        Wait-ProcessBounded $encoderProcess ($EncoderBroadcastSeconds + 45) 'Encoder'
    }
    finally
    {
        if ($null -ne $decoderProcess -and -not $decoderProcess.HasExited)
        {
            $decoderProcess.Kill($true)
            $decoderProcess.WaitForExit()
        }
        if (-not $encoderProcess.HasExited)
        {
            $encoderProcess.Kill($true)
            $encoderProcess.WaitForExit()
        }
    }

    if ($encoderProcess.ExitCode -ne 0 -or $decoderProcess.ExitCode -ne 0)
    {
        throw "Application smoke failed for $($case.Name): encoder=$($encoderProcess.ExitCode), decoder=$($decoderProcess.ExitCode)"
    }
    if (-not $encoderAliveWhenDecoderCompleted)
    {
        throw "Encoder was not still broadcasting when Decoder completed: $($case.Name)"
    }
    $encoderJson = Get-Content -LiteralPath $encoderReport -Raw | ConvertFrom-Json
    $decoderJson = Get-Content -LiteralPath $decoderReport -Raw | ConvertFrom-Json
    $outputFiles = @(Get-ChildItem -LiteralPath $outputRoot -File)
    if ($outputFiles.Count -ne 1 -or $outputFiles[0].Extension -cne '.bin' -or
        @($outputFiles | Where-Object { $_.Name.EndsWith('.part', [StringComparison]::OrdinalIgnoreCase) }).Count -ne 0)
    {
        throw "Expected one published .bin and no .part: $($case.Name)"
    }
    $output = $outputFiles[0].FullName
    $sourceLength = (Get-Item -LiteralPath $case.Source).Length
    $outputLength = (Get-Item -LiteralPath $output).Length
    $sourceSha256 = (Get-FileHash -LiteralPath $case.Source -Algorithm SHA256).Hash.ToLowerInvariant()
    $outputSha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedBackend = if ($case.Backend -ceq 'wgc') { 'WGC' } else { 'DXGI Desktop Duplication' }
    $verifiedEncodedGoodput = [double]$decoderJson.verifiedEncodedGoodputBitsPerSecond
    $captureFps = [double]$decoderJson.captureFps
    $bootstrapSuccessRate = [double]$decoderJson.bootstrapSuccessRate
    $admittedFrameSequenceFps = [double]$decoderJson.admittedFrameSequenceFps
    $minimumBroadcastMilliseconds = [UInt64]$EncoderBroadcastSeconds * 1000
    $maximumBroadcastMilliseconds = [UInt64]($EncoderBroadcastSeconds + 10) * 1000
    if ($sourceLength -ne $outputLength -or $sourceSha256 -cne $outputSha256 -or
        [string]$encoderJson.wholeFileDigest -cne [string]$decoderJson.wholeFileDigest -or
        [string]$decoderJson.state -cne 'Completed' -or -not [bool]$decoderJson.wholeFileDigestVerified -or
        -not [bool]$decoderJson.finalPublishSucceeded -or [string]$encoderJson.state -cne 'Stopped' -or
        $null -ne $encoderJson.receiverProgress -or $null -ne $encoderJson.receiverEta -or
        $null -ne $encoderJson.verifiedGoodput -or [string]$decoderJson.actualBackend -cne $expectedBackend -or
        [int]$encoderJson.dataWindow.left -ne $MonitorOriginX -or [int]$encoderJson.dataWindow.top -ne $MonitorOriginY -or
        [int]$decoderJson.roi.left -ne $MonitorOriginX -or [int]$decoderJson.roi.top -ne $MonitorOriginY -or
        [int]$decoderJson.roi.width -ne 1920 -or [int]$decoderJson.roi.height -ne 1080 -or
        [string]$decoderJson.verifiedRawGoodputBasis -cne 'verifiedRawBytes' -or
        [string]$decoderJson.verifiedEncodedGoodputGate -cne 'WholeFileDigest+finalPublish' -or
        [UInt64]$encoderJson.broadcastRuntimeMilliseconds -lt $minimumBroadcastMilliseconds -or
        [UInt64]$encoderJson.broadcastRuntimeMilliseconds -gt $maximumBroadcastMilliseconds -or
        [UInt64]$decoderJson.verifiedEncodedBytes -eq 0 -or
        [double]::IsNaN($verifiedEncodedGoodput) -or [double]::IsInfinity($verifiedEncodedGoodput) -or
        $verifiedEncodedGoodput -le 0 -or [UInt64]$decoderJson.telemetryCapturedFrames -lt 2 -or
        [double]::IsNaN($captureFps) -or [double]::IsInfinity($captureFps) -or $captureFps -le 0 -or
        [UInt64]$decoderJson.fingerprintedFrames -ne 0 -or $null -ne $decoderJson.uniqueVisualFps -or
        [UInt64]$decoderJson.telemetryBootstrapAttempts -lt 2 -or
        [UInt64]$decoderJson.telemetryBootstrapSuccesses -eq 0 -or
        [double]::IsNaN($bootstrapSuccessRate) -or [double]::IsInfinity($bootstrapSuccessRate) -or
        $bootstrapSuccessRate -le 0 -or $bootstrapSuccessRate -gt 1 -or
        [double]::IsNaN($admittedFrameSequenceFps) -or [double]::IsInfinity($admittedFrameSequenceFps) -or
        $admittedFrameSequenceFps -le 0)
    {
        throw "Authoritative state or digest mismatch: $($case.Name)"
    }
    if ($null -ne $case.ExpectedCodec -and [string]$encoderJson.compressionCodec -cne $case.ExpectedCodec)
    {
        throw "Unexpected compression binding for $($case.Name): $($encoderJson.compressionCodec)"
    }
    $results.Add([ordered]@{
        name = $case.Name
        source = $case.Source
        output = $output
        bytes = $sourceLength
        compressionRequested = $case.Compression
        compressionCodec = $encoderJson.compressionCodec
        backend = $decoderJson.actualBackend
        profile = $case.Profile
        encoderAliveWhenDecoderCompleted = $encoderAliveWhenDecoderCompleted
        encoderStateAfterUserStop = $encoderJson.state
        decoderState = $decoderJson.state
        wholeFileDigest = $decoderJson.wholeFileDigest
        sourceSha256 = $sourceSha256
        outputSha256 = $outputSha256
        sourceOutputLengthEqual = $sourceLength -eq $outputLength
        sourceOutputSha256Equal = $sourceSha256 -ceq $outputSha256
        wholeFileDigestEqual = [string]$encoderJson.wholeFileDigest -ceq [string]$decoderJson.wholeFileDigest
        verifiedEncodedBytes = [UInt64]$decoderJson.verifiedEncodedBytes
        verifiedEncodedGoodputBitsPerSecond = $verifiedEncodedGoodput
        captureFps = $captureFps
        uniqueVisualFps = $decoderJson.uniqueVisualFps
        fingerprintedFrames = [UInt64]$decoderJson.fingerprintedFrames
        admittedFrameSequenceFps = $admittedFrameSequenceFps
        bootstrapSuccessRate = $bootstrapSuccessRate
        encoderReport = $encoderReport
        decoderReport = $decoderReport
    })
    Write-Host "PB_APPLICATION_SMOKE_CASE_PASS name=$($case.Name) bytes=$sourceLength codec=$($encoderJson.compressionCodec) backend=$($decoderJson.actualBackend)"
}

$summary = [ordered]@{
    schema = 'PixelBridge.LocalDesktopApplicationSmoke.1'
    status = 'PASS'
    sourceRoot = $SourceRoot
    buildRoot = $BuildRoot
    evidenceRoot = $EvidenceRoot
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    monitorOrigin = [ordered]@{ x=$MonitorOriginX; y=$MonitorOriginY }
    roi = [ordered]@{ left=$MonitorOriginX; top=$MonitorOriginY; right=$roiRight; bottom=$roiBottom }
    encoderBroadcastSeconds = $EncoderBroadcastSeconds
    decoderTimeoutSeconds = $DecoderTimeoutSeconds
    cases = @($results)
}
$summaryPath = Join-Path $EvidenceRoot 'summary.json'
$summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "PB_APPLICATION_SMOKE_PASS cases=$($results.Count) summary=$summaryPath"

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$EvidenceDirectory,

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-GateCondition
{
    param([bool]$Condition, [string]$Message)
    if (-not $Condition)
    {
        throw "GPU parity evidence validation failed: $Message"
    }
}

function Write-NewUtf8File
{
    param([string]$Path, [string]$Content)
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
        try
        {
            $writer.Write($Content)
            $writer.Flush()
            $stream.Flush($true)
        }
        finally
        {
            $writer.Dispose()
        }
    }
    finally
    {
        $stream.Dispose()
    }
}

function Get-AdapterKey
{
    param($Record)
    return '{0}|{1}|{2}' -f $Record.adapter.backend, $Record.adapter.luidHigh, $Record.adapter.luidLow
}

$resolvedBuild = [System.IO.Path]::GetFullPath($BuildDirectory)
$resolvedEvidence = [System.IO.Path]::GetFullPath($EvidenceDirectory)
if (-not (Test-Path -LiteralPath $resolvedBuild -PathType Container))
{
    throw "Build directory does not exist: $resolvedBuild"
}
if (Test-Path -LiteralPath $resolvedEvidence)
{
    throw "Create-only evidence directory already exists: $resolvedEvidence"
}

$testExecutable = Join-Path $resolvedBuild "tests\PBDemodD3D11\$Configuration\PBDemodD3D11Tests.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf))
{
    throw "GPU parity test executable does not exist: $testExecutable"
}

$null = New-Item -ItemType Directory -Path $resolvedEvidence
$gateLogPath = Join-Path $resolvedEvidence 'gate.log'
$capturedLines = @(& $testExecutable '[.gpu-parity]' --reporter console --durations yes 2>&1 | ForEach-Object { $_.ToString() })
$testExitCode = $LASTEXITCODE
$gateLog = ($capturedLines -join "`n") + "`n"
Write-NewUtf8File -Path $gateLogPath -Content $gateLog
if ($testExitCode -ne 0)
{
    throw "GPU parity test failed with exit code $testExitCode; preserved output at $gateLogPath"
}

$prefix = 'PB_GPU_PARITY_JSON='
$records = @($capturedLines | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) } |
    ForEach-Object { $_.Substring($prefix.Length) | ConvertFrom-Json })
$availabilityRecords = @($records | Where-Object { $_.schema -eq 'PixelBridge.RemoteVisualGpuParity.AdapterAvailability.1' })
$resultRecords = @($records | Where-Object { $_.schema -eq 'PixelBridge.RemoteVisualGpuParity.Result.1' })
$runRecords = @($records | Where-Object { $_.schema -eq 'PixelBridge.RemoteVisualGpuParity.Run.1' })
$summaryRecords = @($records | Where-Object { $_.schema -eq 'PixelBridge.RemoteVisualGpuParity.Summary.1' })
$knownRecordCount = $availabilityRecords.Count + $resultRecords.Count + $runRecords.Count + $summaryRecords.Count
Assert-GateCondition ($records.Count -eq $knownRecordCount) 'unknown JSON schema record was emitted'
Assert-GateCondition ($summaryRecords.Count -eq 1) 'exactly one summary record is required'
$summary = $summaryRecords[0]

$warpAvailability = @($availabilityRecords | Where-Object { $_.adapter.backend -eq 'warp' })
$hardwareAvailability = @($availabilityRecords | Where-Object { $_.adapter.backend -eq 'hardware' })
$availableHardware = @($hardwareAvailability | Where-Object { $_.available -eq $true })
Assert-GateCondition ($warpAvailability.Count -eq 2) 'WARP generation zero and recreate availability records are required'
Assert-GateCondition (@($warpAvailability | Where-Object { $_.available -ne $true }).Count -eq 0) 'WARP must be available'
Assert-GateCondition ([UInt32]$summary.hardwareAdaptersEnumerated -eq $hardwareAvailability.Count) 'hardware enumeration count mismatch'
Assert-GateCondition ([UInt32]$summary.hardwareAdaptersAvailable -eq $availableHardware.Count) 'hardware availability count mismatch'
Assert-GateCondition ($summary.allAvailableAdaptersPassed -eq $true) 'summary did not close all available adapters'

$expectedBackends = 1 + $availableHardware.Count
Assert-GateCondition ($resultRecords.Count -eq 5 * $expectedBackends) 'each available backend must emit four corpus and one recreate result'
Assert-GateCondition ($runRecords.Count -eq 2 * $expectedBackends) 'each available backend must emit generation zero and recreate run records'

$expectedGenerationZeroScenarios = @('blur-gaussian-3x3', 'exact', 'scale-area', 'stale-region')
$generationZeroGroups = @($resultRecords | Where-Object { [UInt32]$_.deviceGeneration -eq 0 } | Group-Object { Get-AdapterKey $_ })
$generationOneGroups = @($resultRecords | Where-Object { [UInt32]$_.deviceGeneration -eq 1 } | Group-Object { Get-AdapterKey $_ })
Assert-GateCondition ($generationZeroGroups.Count -eq $expectedBackends) 'generation-zero backend count mismatch'
Assert-GateCondition ($generationOneGroups.Count -eq $expectedBackends) 'recreated backend count mismatch'
foreach ($group in $generationZeroGroups)
{
    $actualScenarios = @($group.Group.scenario | Sort-Object)
    Assert-GateCondition (($actualScenarios -join ',') -eq ($expectedGenerationZeroScenarios -join ',')) "generation-zero corpus mismatch for $($group.Name)"
}
foreach ($group in $generationOneGroups)
{
    Assert-GateCondition ($group.Count -eq 1 -and $group.Group[0].scenario -eq 'exact') "recreate corpus mismatch for $($group.Name)"
}

foreach ($result in $resultRecords)
{
    Assert-GateCondition ($result.evaluation.evaluated -eq $true) 'evaluation was not performed'
    Assert-GateCondition ($result.evaluation.paddingValid -eq $true) 'padding validation failed'
    Assert-GateCondition ([UInt32]$result.evaluation.codewords -eq 4) 'codeword count mismatch'
    Assert-GateCondition ([UInt32]$result.evaluation.fecFailures -eq 0) 'FEC failure was accepted'
    Assert-GateCondition ([UInt32]$result.evaluation.crcFailures -eq 0) 'CRC failure was accepted'
    Assert-GateCondition ([UInt32]$result.evaluation.identityFailures -eq 0) 'identity failure was accepted'
    Assert-GateCondition ([UInt32]$result.evaluation.falseAcceptedCodewords -eq 0) 'false acceptance was observed'
    Assert-GateCondition ([UInt32]$result.evaluation.acceptedTransportBlocks -eq 4) 'accepted Transport count mismatch'
    Assert-GateCondition ([UInt32]$result.evaluation.acceptedRemoteControlBlocks -eq 0) 'unexpected Remote Control acceptance'
    Assert-GateCondition ($result.acceptedManifest.cpuSetBlake3 -ceq $result.acceptedManifest.gpuSetBlake3) 'CPU/GPU accepted-set digest mismatch'
    Assert-GateCondition (@($result.acceptedManifest.blocks).Count -eq 4) 'accepted-block manifest must contain four blocks'
    Assert-GateCondition ([UInt64]$result.metrics.samples -eq 64800) 'compact metric sample count mismatch'
    Assert-GateCondition ([UInt64]$result.readbackBytes -eq 261040) 'compact readback byte count mismatch'
    Assert-GateCondition ($result.gpuTimingValid -eq $true -and [UInt64]$result.gpuTime100ns -gt 0) 'GPU timestamp is unavailable or zero'
}

foreach ($availability in $availabilityRecords)
{
    if ($null -ne $availability.adapter.driverVersionRaw)
    {
        Assert-GateCondition ($availability.adapter.driverVersionRaw -is [string] -and $availability.adapter.driverVersionRaw -cmatch '^[0-9]+$') 'driverVersionRaw must be a lossless decimal string'
    }
}

foreach ($scenario in $expectedGenerationZeroScenarios)
{
    $digests = @($resultRecords | Where-Object { [UInt32]$_.deviceGeneration -eq 0 -and $_.scenario -eq $scenario } |
        ForEach-Object { $_.acceptedManifest.gpuSetBlake3 } | Sort-Object -Unique)
    Assert-GateCondition ($digests.Count -eq 1) "accepted Transport set differs across adapters for scenario $scenario"
}
$recreatedDigests = @($resultRecords | Where-Object { [UInt32]$_.deviceGeneration -eq 1 -and $_.scenario -eq 'exact' } |
    ForEach-Object { $_.acceptedManifest.gpuSetBlake3 } | Sort-Object -Unique)
Assert-GateCondition ($recreatedDigests.Count -eq 1) 'device-recreate accepted Transport set differs across adapters'

foreach ($run in $runRecords)
{
    Assert-GateCondition ($run.wrongAdapterLuidRejected -eq $true) 'wrong adapter LUID was not rejected'
    Assert-GateCondition ([UInt64]$run.submittedFrames -eq [UInt64]$run.scenarios) 'submitted frame count mismatch'
    Assert-GateCondition ([UInt64]$run.completedFrames -eq [UInt64]$run.scenarios) 'completed frame count mismatch'
    Assert-GateCondition ([UInt64]$run.failedFrames -eq 0 -and [UInt64]$run.cancelledFrames -eq 0) 'run contained failed or cancelled work'
    Assert-GateCondition ([UInt64]$run.metricReadbackBytes -eq [UInt64]$run.scenarios * 261040) 'run compact readback total mismatch'
    Assert-GateCondition ([UInt64]$run.rawPixelReadbackBytes -eq 0) 'run performed ROI-sized pixel readback'
    Assert-GateCondition ([UInt64]$run.gpuTimingSamples -eq [UInt64]$run.scenarios -and [UInt64]$run.gpuTimingUnavailable -eq 0) 'run did not record every GPU timestamp'
    Assert-GateCondition ($run.deviceRemovedHresult -ceq '0x00000000') 'device removal was observed'
    Assert-GateCondition ($run.shutdownCompleted -eq $true) 'demodulator shutdown did not complete'
}

$nvidiaStatus = if (@($availableHardware | Where-Object { [UInt32]$_.adapter.vendorId -eq 0x10DE }).Count -gt 0) { 'pass' } else { 'unavailable' }
$amdStatus = if (@($availableHardware | Where-Object { [UInt32]$_.adapter.vendorId -eq 0x1002 }).Count -gt 0) { 'pass' } else { 'unavailable' }
Assert-GateCondition ($summary.nvidia -ceq $nvidiaStatus) 'NVIDIA availability summary mismatch'
Assert-GateCondition ($summary.amd -ceq $amdStatus) 'AMD availability summary mismatch'

$cachePath = Join-Path $resolvedBuild 'CMakeCache.txt'
$sourceDirectory = $null
$sourceHead = $null
$sourceStatus = @()
if (Test-Path -LiteralPath $cachePath -PathType Leaf)
{
    $homeEntry = Get-Content -LiteralPath $cachePath | Where-Object { $_.StartsWith('CMAKE_HOME_DIRECTORY:INTERNAL=', [StringComparison]::Ordinal) } | Select-Object -First 1
    if ($homeEntry)
    {
        $sourceDirectory = $homeEntry.Substring('CMAKE_HOME_DIRECTORY:INTERNAL='.Length)
        if (Test-Path -LiteralPath (Join-Path $sourceDirectory '.git'))
        {
            $sourceHead = (& git -C $sourceDirectory rev-parse HEAD).Trim()
            if ($LASTEXITCODE -ne 0)
            {
                throw 'Unable to resolve source HEAD'
            }
            $sourceStatus = @(& git -C $sourceDirectory status --short)
            if ($LASTEXITCODE -ne 0)
            {
                throw 'Unable to capture source status'
            }
        }
    }
}

$executableItem = Get-Item -LiteralPath $testExecutable
$gateLogItem = Get-Item -LiteralPath $gateLogPath
$validatorPath = [System.IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$validatorItem = Get-Item -LiteralPath $validatorPath
$evidence = [ordered]@{
    schema = 'PixelBridge.RemoteVisualGpuParity.Evidence.1'
    collectedUtc = [DateTime]::UtcNow.ToString('o')
    command = [ordered]@{
        executable = $testExecutable
        arguments = @('[.gpu-parity]', '--reporter', 'console', '--durations', 'yes')
        exitCode = $testExitCode
    }
    executable = [ordered]@{
        size = [UInt64]$executableItem.Length
        sha256 = (Get-FileHash -LiteralPath $testExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    validator = [ordered]@{
        path = $validatorPath
        size = [UInt64]$validatorItem.Length
        sha256 = (Get-FileHash -LiteralPath $validatorPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    source = [ordered]@{
        directory = $sourceDirectory
        head = $sourceHead
        statusShort = $sourceStatus
    }
    validation = [ordered]@{
        jsonRecords = $records.Count
        availabilityRecords = $availabilityRecords.Count
        resultRecords = $resultRecords.Count
        runRecords = $runRecords.Count
        allGpuTimestampsValid = $true
        allWrongAdapterLuidsRejected = $true
        allCpuGpuAcceptedSetsIdentical = $true
        allAvailableAdaptersPassed = $true
    }
    summary = $summary
    adapters = @($availabilityRecords | Group-Object { Get-AdapterKey $_ } | ForEach-Object { $_.Group[0].adapter })
    artifacts = [ordered]@{
        gateLog = [ordered]@{
            file = 'gate.log'
            size = [UInt64]$gateLogItem.Length
            sha256 = (Get-FileHash -LiteralPath $gateLogPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}
$evidencePath = Join-Path $resolvedEvidence 'evidence.json'
Write-NewUtf8File -Path $evidencePath -Content (($evidence | ConvertTo-Json -Depth 12) + "`n")

$hashLines = @()
foreach ($fileName in @('gate.log', 'evidence.json'))
{
    $filePath = Join-Path $resolvedEvidence $fileName
    $hashLines += '{0}  {1}' -f (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant(), $fileName
}
Write-NewUtf8File -Path (Join-Path $resolvedEvidence 'sha256s.txt') -Content (($hashLines -join "`n") + "`n")

Write-Output ('GPU parity gate PASS: adapters={0}, results={1}, evidence={2}' -f $expectedBackends, $resultRecords.Count, $resolvedEvidence)

#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MatrixSpecPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedMatrixSpecSha256,

    [string]$HardwareScopePath = '',

    [AllowEmptyString()]
    [string]$ExpectedHardwareScopeSha256 = '',

    [Parameter(Mandatory = $true)]
    [ValidateCount(1, 256)]
    [string[]]$RunEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

function Write-CreateOnlyUtf8Text
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $partial = "$resolved.partial"
    if ((Test-Path -LiteralPath $resolved) -or (Test-Path -LiteralPath $partial))
    {
        throw "Create-only matrix output already exists: $resolved"
    }
    $bytes = [System.Text.UTF8Encoding]::new($false, $true).GetBytes($Text)
    $stream = [System.IO.File]::Open($partial, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally
    {
        $stream.Dispose()
    }
    try
    {
        [System.IO.File]::Move($partial, $resolved)
    }
    catch
    {
        throw "Create-only matrix output publication failed; partial retained at $partial"
    }
}

function Get-SealedRun
{
    param([Parameter(Mandatory = $true)][string]$Directory)
    $root = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $rootItem = Get-Item -LiteralPath $root -Force -ErrorAction Stop
    if (-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint))
    {
        throw 'Matrix run evidence root must be an existing non-reparse directory'
    }
    $recordPath = Join-Path $root 'matrix-run-record.json'
    $record = Import-PBRemoteVisualMatrixRunRecord -Path $recordPath
    $successSealPath = Join-Path $root 'pilot-evidence-seal.json'
    $failureSealPath = Join-Path $root 'failure-evidence-seal.json'
    $hasSuccessSeal = Test-Path -LiteralPath $successSealPath -PathType Leaf
    $hasFailureSeal = Test-Path -LiteralPath $failureSealPath -PathType Leaf
    if ($hasSuccessSeal -eq $hasFailureSeal)
    {
        throw 'Matrix run evidence must contain exactly one success or classified-failure seal'
    }
    $sealPath = if ($hasSuccessSeal) { $successSealPath } else { $failureSealPath }
    $seal = Read-PBBoundedJson -Path $sealPath -MaximumBytes 32MB
    $expectedSchema = if ([string]$record.value.outcome -ceq 'Success')
    {
        'PixelBridge.RemoteVisualPilotEvidenceSeal.1'
    }
    else
    {
        'PixelBridge.RemoteVisualFieldFailureSeal.1'
    }
    if ([string]$seal.schema -cne $expectedSchema -or [string]$seal.status -cne 'PASS' -or
        [string]$seal.runId -cne [string]$record.value.runId -or $seal.artifactCount -is [bool] -or
        [UInt64]$seal.artifactCount -ne @($seal.artifacts).Count -or @($seal.artifacts).Count -eq 0 -or
        @($seal.artifacts).Count -gt 256)
    {
        throw 'Matrix run evidence seal schema, status, RunId, or artifact count is invalid'
    }
    if ([string]$record.value.outcome -ceq 'Failure' -and
        [string]$seal.failureClassification -cne [string]$record.value.failureClassification)
    {
        throw 'Matrix failure seal classification differs from its run record'
    }
    $sealedPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $recordSealed = $false
    foreach ($artifact in @($seal.artifacts))
    {
        Assert-PBIdentityShape -Identity $artifact -Name 'matrix run sealed artifact'
        $path = [System.IO.Path]::GetFullPath([string]$artifact.path)
        if (-not $sealedPaths.Add($path))
        {
            throw 'Matrix run seal contains a duplicate artifact path'
        }
        [void](Assert-PBFileIdentity -Path $path -Expected $artifact -Name 'matrix run sealed artifact')
        if ($path -ieq $record.path -and [UInt64]$artifact.size -eq [UInt64]$record.identity.size -and
            [string]$artifact.sha256 -ceq [string]$record.identity.sha256)
        {
            $recordSealed = $true
        }
    }
    if (-not $recordSealed)
    {
        throw 'Matrix run record is not included in its immutable evidence seal'
    }
    foreach ($identityName in @('plan', 'deployment', 'packageManifest', 'source', 'remoteUiEvidence',
        'encoderEnvironment', 'decoderEnvironment', 'replay', 'combinedReport', 'outcomeVerification',
        'encoderReport', 'liveDecoderReport', 'offlineDecoderReport'))
    {
        $identity = $record.value.identities[$identityName]
        $path = [System.IO.Path]::GetFullPath([string]$identity.path)
        if (-not $sealedPaths.Contains($path))
        {
            throw "Matrix run seal omits record identity '$identityName'"
        }
    }
    if ($null -ne $record.value.identities.inspection -and
        -not $sealedPaths.Contains([System.IO.Path]::GetFullPath([string]$record.value.identities.inspection.path)))
    {
        throw 'Matrix failure seal omits its Replay inspection'
    }
    return [ordered]@{
        root = $root
        recordPath = $record.path
        recordIdentity = $record.identity
        record = $record.value
        plan = $record.plan.value
        combined = $record.combined
        sealPath = $sealPath
        sealIdentity = Get-PBFileIdentity -Path $sealPath
    }
}

function Get-RunTupleKey
{
    param([Parameter(Mandatory = $true)][object]$Run)
    return "$([string]$Run.record.matrix.modeClass)|$([string]$Run.record.matrix.captureBackend)|$([UInt32]$Run.record.matrix.logicalFps)|$([string]$Run.record.matrix.scaleTarget)"
}

function Get-ComparisonMetricNames
{
    param([Parameter(Mandatory = $true)][object[]]$Runs)
    $names = [Collections.Generic.List[string]]::new()
    foreach ($name in @('verifiedEncodedGoodputBitsPerSecond', 'bootstrapSuccessRate', 'fecFrameErrorRate',
        'fecCodewordFailureRate', 'uniqueVisualFps', 'endToEndUniqueVisualFps'))
    {
        $available = $true
        foreach ($run in $Runs)
        {
            if ($null -eq $run.record.authoritativeMetrics[$name])
            {
                $available = $false
                break
            }
        }
        if ($available)
        {
            [void]$names.Add($name)
        }
    }
    return @($names)
}

function New-ComparisonProfile
{
    param([Parameter(Mandatory = $true)][object]$Run)
    $metrics = $Run.record.authoritativeMetrics
    return [ordered]@{
        runId = [string]$Run.record.runId
        outcome = [string]$Run.record.outcome
        failureClassification = [string]$Run.record.failureClassification
        profileToken = [string]$Run.record.profileToken
        verifiedEncodedGoodputBitsPerSecond = $metrics.verifiedEncodedGoodputBitsPerSecond
        bootstrapSuccessRate = $metrics.bootstrapSuccessRate
        fecFrameErrorRate = $metrics.fecFrameErrorRate
        fecCodewordFailureRate = $metrics.fecCodewordFailureRate
        uniqueVisualFps = $metrics.uniqueVisualFps
        endToEndUniqueVisualFps = $metrics.endToEndUniqueVisualFps
        temporallyAdmittedTransportBlocks = [UInt64]$metrics.temporallyAdmittedTransportBlocks
        wholeFileDigestVerified = [bool]$metrics.wholeFileDigestVerified
        finalPublishSucceeded = [bool]$metrics.finalPublishSucceeded
        combinedReport = $Run.record.identities.combinedReport
    }
}

function New-MatrixCsvRow
{
    param(
        [Parameter(Mandatory = $true)][object]$Run,
        [Parameter(Mandatory = $true)][string]$CellId
    )
    $record = $Run.record
    $metrics = $record.authoritativeMetrics
    return [pscustomobject][ordered]@{
        runId = [string]$record.runId
        matrixCellId = $CellId
        outcome = [string]$record.outcome
        failureClassification = [string]$record.failureClassification
        profileToken = [string]$record.profileToken
        profileName = [string]$record.profileName
        modeClass = [string]$record.matrix.modeClass
        visibleRemoteMode = [string]$record.matrix.visibleRemoteMode
        captureBackend = [string]$record.matrix.captureBackend
        scaleTarget = [string]$record.matrix.scaleTarget
        estimatedScaleX = [double]$record.matrix.estimatedScaleX
        estimatedScaleY = [double]$record.matrix.estimatedScaleY
        logicalFps = [UInt32]$record.matrix.logicalFps
        geometryMode = [string]$record.matrix.geometryMode
        successfulRun = [bool]$metrics.successfulRun
        evidenceValid = [bool]$metrics.evidenceValid
        encoderState = [string]$metrics.encoderState
        decoderState = [string]$metrics.decoderState
        generatedVisualFramesPerSecond = $metrics.generatedVisualFramesPerSecond
        generatedPayloadBytesPerSecond = $metrics.generatedPayloadBytesPerSecond
        captureFps = $metrics.captureFps
        uniqueVisualFps = $metrics.uniqueVisualFps
        endToEndUniqueVisualFps = $metrics.endToEndUniqueVisualFps
        bootstrapSuccessRate = $metrics.bootstrapSuccessRate
        preFecBerEstimate = $metrics.preFecBerEstimate
        fecFrameErrorRate = $metrics.fecFrameErrorRate
        fecCodewordFailureRate = $metrics.fecCodewordFailureRate
        evaluatedDataFrames = [UInt64]$metrics.evaluatedDataFrames
        evaluatedCodewords = [UInt64]$metrics.evaluatedCodewords
        fecAcceptedTransportBlocks = [UInt64]$metrics.fecAcceptedTransportBlocks
        temporallyAdmittedTransportBlocks = [UInt64]$metrics.temporallyAdmittedTransportBlocks
        remoteMetricFrames = [UInt64]$metrics.remoteMetricFrames
        remoteMetricSamples = [UInt64]$metrics.remoteMetricSamples
        remoteMetricZeroMagnitudeRate = $metrics.remoteMetricZeroMagnitudeRate
        remoteMetricMeanAbsoluteMetric = $metrics.remoteMetricMeanAbsoluteMetric
        remoteSymbolSamples = [UInt64]$metrics.remoteSymbolSamples
        remoteUnreliableSymbols = [UInt64]$metrics.remoteUnreliableSymbols
        remoteUnreliableSymbolRate = $metrics.remoteUnreliableSymbolRate
        remoteRejectedMetricFrames = [UInt64]$metrics.remoteRejectedMetricFrames
        remoteStaleRegions = [UInt64]$metrics.remoteStaleRegions
        remoteFreshnessTagMismatches = [UInt64]$metrics.remoteFreshnessTagMismatches
        remoteFreshnessTagErasures = [UInt64]$metrics.remoteFreshnessTagErasures
        duplicateFrameSequences = [UInt64]$metrics.duplicateFrameSequences
        reorderedFrameSequences = [UInt64]$metrics.reorderedFrameSequences
        frameSequenceGapEvents = [UInt64]$metrics.frameSequenceGapEvents
        skippedFrameSequences = [UInt64]$metrics.skippedFrameSequences
        captureStallCount = [UInt64]$metrics.captureStallCount
        visualStallCount = [UInt64]$metrics.visualStallCount
        verifiedEncodedGoodputBitsPerSecond = $metrics.verifiedEncodedGoodputBitsPerSecond
        wholeFileDigestVerified = [bool]$metrics.wholeFileDigestVerified
        finalPublishSucceeded = [bool]$metrics.finalPublishSucceeded
        externalMatch = $metrics.externalMatch
        falseAcceptedCodewords = $metrics.falseAcceptedCodewords
        outerConflictRejections = [UInt64]$metrics.outerConflictRejections
        planSha256 = [string]$record.identities.plan.sha256
        replaySha256 = [string]$record.identities.replay.sha256
        combinedReportSha256 = [string]$record.identities.combinedReport.sha256
        matrixRunRecordSha256 = [string]$Run.recordIdentity.sha256
        evidenceSealSha256 = [string]$Run.sealIdentity.sha256
    }
}

$matrixSpec = Import-PBRemoteVisualStep21MatrixSpec -Path $MatrixSpecPath -ExpectedSha256 $ExpectedMatrixSpecSha256
$hasHardwareScopePath = -not [string]::IsNullOrWhiteSpace($HardwareScopePath)
$hasHardwareScopeSha256 = -not [string]::IsNullOrWhiteSpace($ExpectedHardwareScopeSha256)
if ($hasHardwareScopePath -ne $hasHardwareScopeSha256 -or
    ($hasHardwareScopeSha256 -and $ExpectedHardwareScopeSha256 -cnotmatch '^[0-9a-f]{64}$'))
{
    throw 'Step 21 hardware scope path and expected SHA-256 must be supplied together'
}
$hardwareScope = $null
$coverageCells = @($matrixSpec.value.cells)
$expectedRunCount = [UInt32]$matrixSpec.value.cellCount
if ($hasHardwareScopePath)
{
    $hardwareScope = Import-PBRemoteVisualStep21HardwareScope -Path $HardwareScopePath `
        -ExpectedSha256 $ExpectedHardwareScopeSha256
    Assert-PBMatrixIdentityEqual -Actual $hardwareScope.value.matrixSpecification -Expected $matrixSpec.identity `
        -Name 'Step 21 hardware scope parent MatrixSpec'
    $coverageCells = @($hardwareScope.value.includedCells)
    $expectedRunCount = [UInt32]$hardwareScope.value.includedCellCount
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrEmpty($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container) -or
    (Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath "$resolvedOutput.partial"))
{
    throw 'Step 21 matrix output must be a new path below an existing parent'
}
$runs = [Collections.Generic.List[object]]::new()
$roots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($directory in $RunEvidenceDirectory)
{
    $run = Get-SealedRun -Directory $directory
    foreach ($existingRoot in $roots)
    {
        if (([string]$run.root).StartsWith([string]$existingRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
            ([string]$existingRoot).StartsWith([string]$run.root + '\', [StringComparison]::OrdinalIgnoreCase))
        {
            throw 'Step 21 matrix input evidence directories must not contain one another'
        }
    }
    if (-not $roots.Add([string]$run.root))
    {
        throw 'Step 21 matrix input repeats an evidence directory'
    }
    [void]$runs.Add($run)
}
if ($runs.Count -gt 256)
{
    throw 'Step 21 matrix exceeds the 256-run evidence bound'
}
foreach ($root in $roots)
{
    if ($resolvedOutput.StartsWith([string]$root + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Independent Step 21 matrix output must remain outside every sealed run evidence root'
    }
}

if ($runs.Count -ne $expectedRunCount)
{
    throw "Step 21 matrix requires exactly $expectedRunCount run records for this frozen acceptance scope"
}

$runIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$planHashes = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$deploymentHashes = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$uiHashes = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$replayPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$replayIdentities = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$combinedPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$combinedIdentities = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$encoderEnvironmentPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$encoderEnvironmentIdentities = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$decoderEnvironmentPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$decoderEnvironmentIdentities = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$packageKey = $null
$sourceKey = $null
foreach ($run in $runs)
{
    $record = $run.record
    if (-not $runIds.Add([string]$record.runId) -or -not $planHashes.Add([string]$record.identities.plan.sha256) -or
        -not $deploymentHashes.Add([string]$record.identities.deployment.sha256) -or
        -not $uiHashes.Add([string]$record.identities.remoteUiEvidence.sha256) -or
        -not $replayPaths.Add([System.IO.Path]::GetFullPath([string]$record.identities.replay.path)) -or
        -not $replayIdentities.Add("$([UInt64]$record.identities.replay.size)|$([string]$record.identities.replay.sha256)") -or
        -not $combinedPaths.Add([System.IO.Path]::GetFullPath([string]$record.identities.combinedReport.path)) -or
        -not $combinedIdentities.Add("$([UInt64]$record.identities.combinedReport.size)|$([string]$record.identities.combinedReport.sha256)") -or
        -not $encoderEnvironmentPaths.Add([System.IO.Path]::GetFullPath([string]$record.identities.encoderEnvironment.path)) -or
        -not $encoderEnvironmentIdentities.Add("$([UInt64]$record.identities.encoderEnvironment.size)|$([string]$record.identities.encoderEnvironment.sha256)") -or
        -not $decoderEnvironmentPaths.Add([System.IO.Path]::GetFullPath([string]$record.identities.decoderEnvironment.path)) -or
        -not $decoderEnvironmentIdentities.Add("$([UInt64]$record.identities.decoderEnvironment.size)|$([string]$record.identities.decoderEnvironment.sha256)"))
    {
        throw 'Step 21 matrix attempts to merge or reuse RunId, plan, deployment, UI, endpoint environment, Replay, or combined evidence'
    }
    $currentPackage = "$([UInt64]$record.identities.packageManifest.size)|$([string]$record.identities.packageManifest.sha256)"
    $currentSource = "$([UInt64]$record.identities.source.size)|$([string]$record.identities.source.sha256)"
    if ($null -eq $packageKey) { $packageKey = $currentPackage }
    if ($null -eq $sourceKey) { $sourceKey = $currentSource }
    if ($currentPackage -cne $packageKey -or $currentSource -cne $sourceKey)
    {
        throw 'Step 21 matrix runs do not share the exact tested package and frozen source identity'
    }
    $metadata = $run.combined.decoder.remoteMetadata
    if ($null -ne $metadata.observedLatencyMilliseconds)
    {
        throw 'Step 21 matrix refuses an inferred latency value; the current UI evidence contract has no visible latency claim'
    }
    if ([string]$metadata.chromaMode -cne 'Unknown' -and
        @($run.plan.remoteUi.visibleFields) -cnotcontains 'chromaMode')
    {
        throw 'Step 21 matrix refuses a non-Unknown chroma value without UI-visible provenance'
    }
}

$cellByRunId = @{}
foreach ($cell in $coverageCells)
{
    $matches = @($runs | Where-Object {
        $coverageRole = if ([string]$_.record.matrix.profileComparisonRole -ceq 'Baseline')
        {
            'ProfileBaseline'
        }
        else
        {
            [string]$_.record.matrix.backendCoverageRole
        }
        [string]$_.record.profileToken -ceq [string]$cell.profileToken -and
        [string]$_.record.matrix.captureBackend -ceq [string]$cell.captureBackend -and
        [string]$_.record.matrix.modeClass -ceq [string]$cell.modeClass -and
        [string]$_.record.matrix.scaleTarget -ceq [string]$cell.scaleTarget -and
        [UInt32]$_.record.matrix.logicalFps -eq [UInt32]$cell.logicalFps -and
        [string]$_.record.matrix.profileComparisonRole -ceq [string]$cell.profileComparisonRole -and
        $coverageRole -ceq [string]$cell.coverageRole
    })
    if ($matches.Count -ne 1)
    {
        throw "Frozen Step 21 cell '$($cell.cellId)' expected exactly one independent run but found $($matches.Count)"
    }
    $runId = [string]$matches[0].record.runId
    if ($cellByRunId.Contains($runId))
    {
        throw "Step 21 run $runId was mapped to more than one frozen matrix cell"
    }
    $cellByRunId[$runId] = [string]$cell.cellId
}
if ($cellByRunId.Count -ne $runs.Count)
{
    throw 'Step 21 input contains a run outside the frozen matrix specification'
}

$mainSpecCells = @($coverageCells | Where-Object {
    [string]$_.profileToken -ceq 'remote-lf4' -and [string]$_.captureBackend -ceq 'wgc' -and
    [string]$_.coverageRole -ceq 'MainMatrix'
})
$mainCellIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($cell in $mainSpecCells)
{
    if (-not $mainCellIds.Add([string]$cell.cellId))
    {
        throw 'Step 21 acceptance scope contains a duplicate LF4/WGC main cell identity'
    }
}
$mainCellRuns = @($runs | Where-Object { $mainCellIds.Contains([string]$cellByRunId[[string]$_.record.runId]) })
if ($mainCellRuns.Count -ne $mainSpecCells.Count)
{
    throw 'LF4/WGC main matrix does not exactly cover the frozen acceptance scope'
}
$requiredModes = @($mainSpecCells | ForEach-Object { [string]$_.modeClass } | Sort-Object -Unique)
$requiredFps = @($mainSpecCells | ForEach-Object { [UInt32]$_.logicalFps } | Sort-Object -Unique)
$requiredScales = @($mainSpecCells | ForEach-Object { [string]$_.scaleTarget } | Sort-Object -Unique)

$dxgiRuns = @($runs | Where-Object {
    [string]$_.record.profileToken -ceq 'remote-lf4' -and
    [string]$_.record.matrix.captureBackend -ceq 'dxgi' -and
    [string]$_.record.matrix.backendCoverageRole -ceq 'RepresentativeRecheck'
})
$dxgiModes = @($dxgiRuns | ForEach-Object { [string]$_.record.matrix.modeClass } | Sort-Object -Unique)
$dxgiFps = @($dxgiRuns | ForEach-Object { [UInt32]$_.record.matrix.logicalFps } | Sort-Object -Unique)
$dxgiScales = @($dxgiRuns | ForEach-Object { [string]$_.record.matrix.scaleTarget } | Sort-Object -Unique)
$dxgiSpecCells = @($coverageCells | Where-Object {
    [string]$_.profileToken -ceq 'remote-lf4' -and [string]$_.captureBackend -ceq 'dxgi' -and
    [string]$_.coverageRole -ceq 'RepresentativeRecheck'
})
$expectedDxgiModes = @($dxgiSpecCells | ForEach-Object { [string]$_.modeClass } | Sort-Object -Unique)
$expectedDxgiFps = @($dxgiSpecCells | ForEach-Object { [UInt32]$_.logicalFps } | Sort-Object -Unique)
$expectedDxgiScales = @($dxgiSpecCells | ForEach-Object { [string]$_.scaleTarget } | Sort-Object -Unique)
if ($dxgiRuns.Count -ne $dxgiSpecCells.Count -or
    (($dxgiModes | ConvertTo-Json -Compress) -cne ($expectedDxgiModes | ConvertTo-Json -Compress)) -or
    (($dxgiFps | ConvertTo-Json -Compress) -cne ($expectedDxgiFps | ConvertTo-Json -Compress)) -or
    (($dxgiScales | ConvertTo-Json -Compress) -cne ($expectedDxgiScales | ConvertTo-Json -Compress)))
{
    throw 'LF4/DXGI representative recheck does not exactly match the frozen acceptance scope'
}

$directRuns = @($runs | Where-Object { [string]$_.record.profileToken -ceq 'direct' })
$shapeRuns = @($runs | Where-Object { [string]$_.record.profileToken -ceq 'shape' })
if ($directRuns.Count -eq 0 -or $shapeRuns.Count -eq 0)
{
    throw 'Step 21 matrix requires independent Direct and Shape success-or-failure evidence'
}

$comparison = $null
$candidateKeys = @($mainCellRuns | Where-Object { [string]$_.record.matrix.scaleTarget -ceq '1.000' } |
    ForEach-Object { Get-RunTupleKey -Run $_ } | Sort-Object -Unique)
foreach ($key in $candidateKeys)
{
    $lf4 = @($mainCellRuns | Where-Object { (Get-RunTupleKey -Run $_) -ceq $key })
    $direct = @($directRuns | Where-Object { (Get-RunTupleKey -Run $_) -ceq $key })
    $shape = @($shapeRuns | Where-Object { (Get-RunTupleKey -Run $_) -ceq $key })
    if ($lf4.Count -ne 1 -or $direct.Count -ne 1 -or $shape.Count -ne 1)
    {
        continue
    }
    $cohort = @($direct[0], $shape[0], $lf4[0])
    $metricNames = @(Get-ComparisonMetricNames -Runs $cohort)
    if ($metricNames.Count -eq 0)
    {
        continue
    }
    $comparison = [ordered]@{
        tupleKey = $key
        metricBasis = $metricNames
        direct = New-ComparisonProfile -Run $direct[0]
        shape = New-ComparisonProfile -Run $shape[0]
        remoteLf4 = New-ComparisonProfile -Run $lf4[0]
        interpretation = 'Per-run authoritative metrics only; no counters, durations, or denominators were merged across runs'
    }
    break
}
if ($null -eq $comparison)
{
    throw 'Step 21 lacks one exact mode/backend/FPS/1:1 Direct-Shape-LF4 cohort with a shared authoritative metric denominator'
}

New-Item -ItemType Directory -Path $resolvedOutput | Out-Null
$sortedRuns = @($runs | Sort-Object { [string]$_.record.runId })
$csvRows = @($sortedRuns | ForEach-Object { New-MatrixCsvRow -Run $_ -CellId ([string]$cellByRunId[[string]$_.record.runId]) })
$csvText = ($csvRows | ConvertTo-Csv -NoTypeInformation -UseQuotes AsNeeded) -join "`n"
$csvText += "`n"
$csvPath = Join-Path $resolvedOutput 'step21-provider-generic-matrix.csv'
Write-CreateOnlyUtf8Text -Path $csvPath -Text $csvText
$runEntries = @($sortedRuns | ForEach-Object {
    [ordered]@{
        runId = [string]$_.record.runId
        matrixCellId = [string]$cellByRunId[[string]$_.record.runId]
        outcome = [string]$_.record.outcome
        failureClassification = [string]$_.record.failureClassification
        profileToken = [string]$_.record.profileToken
        matrix = $_.record.matrix
        runRecord = $_.recordIdentity
        evidenceSeal = $_.sealIdentity
        combinedReport = $_.record.identities.combinedReport
        replay = $_.record.identities.replay
    }
})
$hardwareScopeSummary = if ($null -eq $hardwareScope)
{
    $null
}
else
{
    [ordered]@{
        scopeId = [string]$hardwareScope.value.scopeId
        scopeClass = [string]$hardwareScope.value.scopeClass
        identity = $hardwareScope.identity
        computerAMonitorCatalog = $hardwareScope.value.computerAMonitorCatalog
        experimentMonitor = $hardwareScope.value.experimentMonitor
        fullCellCount = [UInt32]$hardwareScope.value.fullCellCount
        includedCellCount = [UInt32]$hardwareScope.value.includedCellCount
        excludedCellCount = [UInt32]$hardwareScope.value.excludedCellCount
        excludedCells = @($hardwareScope.value.excludedCells)
    }
}
$summary = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21MatrixEvidence.2'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'PASS'
    runCount = $runs.Count
    matrixSpecification = [ordered]@{
        matrixId = [string]$matrixSpec.value.matrixId
        identity = $matrixSpec.identity
        comparisonModeClass = [string]$matrixSpec.value.comparisonModeClass
        comparisonLogicalFps = [UInt32]$matrixSpec.value.comparisonLogicalFps
        hardwareScope = $hardwareScopeSummary
    }
    commonIdentities = [ordered]@{
        packageManifest = $sortedRuns[0].record.identities.packageManifest
        source = $sortedRuns[0].record.identities.source
    }
    coverage = [ordered]@{
        lf4WgcMain = [ordered]@{
            requiredCells = $mainSpecCells.Count
            exactCells = $mainCellRuns.Count
            modeClasses = $requiredModes
            logicalFps = $requiredFps
            scaleTargets = $requiredScales
        }
        lf4DxgiRepresentative = [ordered]@{
            runCount = $dxgiRuns.Count
            modeClasses = $dxgiModes
            logicalFps = $dxgiFps
            scaleTargets = $dxgiScales
        }
        direct = [ordered]@{ runCount = $directRuns.Count; outcomes = @($directRuns | ForEach-Object { [string]$_.record.outcome } | Sort-Object -Unique) }
        shape = [ordered]@{ runCount = $shapeRuns.Count; outcomes = @($shapeRuns | ForEach-Object { [string]$_.record.outcome } | Sort-Object -Unique) }
        remoteLf4 = [ordered]@{ runCount = @($runs | Where-Object { [string]$_.record.profileToken -ceq 'remote-lf4' }).Count }
    }
    authoritativeComparison = $comparison
    csv = Get-PBFileIdentity -Path $csvPath
    runs = $runEntries
    truthBoundary = [ordered]@{
        providerGeneric = $true
        hardwareScoped = $null -ne $hardwareScope
        full41CoverageCompleted = $null -eq $hardwareScope
        excludedCellsDoNotCountAsCoverage = $null -ne $hardwareScope
        uiVisibleModeEvidenceRequiredPerRun = $true
        unknownChromaAndLatencyNotInferred = $true
        invalidGeometryNeverResampled = $true
        metricsKeptPerRun = $true
        certifiedRemoteVisualProfile = $false
        statement = if ($null -eq $hardwareScope) {
            'PASS means full 41-cell Step 21 matrix coverage and evidence integrity only; it does not set CertifiedRemoteVisualProfile or Step 22 smoke status'
        } else {
            'PASS means exact 31-cell coverage within the sealed dual-2560x1440 hardware scope; the 10 excluded 1.5x cells were not run and are not coverage'
        }
    }
}
$summaryPath = Join-Path $resolvedOutput 'step21-provider-generic-matrix.json'
[void](Write-PBCreateOnlyJson -Path $summaryPath -Value $summary -Depth 50)
$sealArtifacts = [Collections.Generic.List[object]]::new()
foreach ($run in $sortedRuns)
{
    [void]$sealArtifacts.Add($run.recordIdentity)
    [void]$sealArtifacts.Add($run.sealIdentity)
}
[void]$sealArtifacts.Add($matrixSpec.identity)
if ($null -ne $hardwareScope)
{
    [void]$sealArtifacts.Add($hardwareScope.identity)
    [void]$sealArtifacts.Add($hardwareScope.value.computerAMonitorCatalog)
}
[void]$sealArtifacts.Add((Get-PBFileIdentity -Path $csvPath))
[void]$sealArtifacts.Add((Get-PBFileIdentity -Path $summaryPath))
$seal = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21MatrixSeal.2'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'PASS'
    runCount = $runs.Count
    matrixId = [string]$matrixSpec.value.matrixId
    scopeId = if ($null -eq $hardwareScope) { $null } else { [string]$hardwareScope.value.scopeId }
    fullCellCount = [UInt32]$matrixSpec.value.cellCount
    excludedCellCount = if ($null -eq $hardwareScope) { [UInt32]0 } else { [UInt32]$hardwareScope.value.excludedCellCount }
    artifactCount = $sealArtifacts.Count
    artifacts = @($sealArtifacts)
}
$sealPath = Join-Path $resolvedOutput 'step21-provider-generic-matrix.seal.json'
[void](Write-PBCreateOnlyJson -Path $sealPath -Value $seal -Depth 30)
[ordered]@{
    path = $resolvedOutput
    status = 'PASS'
    runCount = $runs.Count
    fullCellCount = [UInt32]$matrixSpec.value.cellCount
    excludedCellCount = if ($null -eq $hardwareScope) { [UInt32]0 } else { [UInt32]$hardwareScope.value.excludedCellCount }
    lf4WgcMainCells = $mainCellRuns.Count
    lf4DxgiRepresentativeRuns = $dxgiRuns.Count
    comparisonTuple = [string]$comparison.tupleKey
    csvSha256 = [string](Get-PBFileIdentity -Path $csvPath).sha256
    summarySha256 = [string](Get-PBFileIdentity -Path $summaryPath).sha256
    sealSha256 = [string](Get-PBFileIdentity -Path $sealPath).sha256
} | ConvertTo-Json -Depth 10

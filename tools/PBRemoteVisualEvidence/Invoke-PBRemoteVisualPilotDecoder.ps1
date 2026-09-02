#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Live', 'Offline')]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [string]$PlanPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPlanSha256,

    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackageSealPath,

    [Parameter(Mandatory = $true)]
    [string]$RemoteMetadataPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$ReplayInputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)
{
    $PSNativeCommandUseErrorActionPreference = $false
}

$commonModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop
$frozenPlan = Import-PBRemoteVisualPilotPlan -Path $PlanPath -ExpectedSha256 $ExpectedPlanSha256
$plan = $frozenPlan.value
$isStep21Plan = [string]$plan.schema -in @('PixelBridge.RemoteVisualPilotPlan.2', 'PixelBridge.RemoteVisualPilotPlan.3')
$expectedActualBackend = if ([string]$plan.policy.captureBackend -ceq 'wgc') { 'WGC' } else { 'DXGI' }
$resolvedPackageDirectory = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedMetadata = [System.IO.Path]::GetFullPath($RemoteMetadataPath)
$resolvedOutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if ($Mode -ceq 'Live' -and -not [string]::IsNullOrWhiteSpace($ReplayInputPath))
{
    throw 'Live Decoder mode does not accept ReplayInputPath'
}
if ($Mode -ceq 'Offline' -and [string]::IsNullOrWhiteSpace($ReplayInputPath))
{
    throw 'Offline Decoder mode requires ReplayInputPath from the completed live run'
}
if (-not (Test-Path -LiteralPath $resolvedPackageDirectory -PathType Container))
{
    throw "Package directory does not exist: $resolvedPackageDirectory"
}
$packageDirectoryItem = Get-Item -LiteralPath $resolvedPackageDirectory
if ($packageDirectoryItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
{
    throw 'Package directory must not be a reparse point'
}
if ((Test-Path -LiteralPath $resolvedOutputDirectory) -or (Test-Path -LiteralPath "$resolvedOutputDirectory.partial"))
{
    throw "Create-only Decoder output directory or partial already exists: $resolvedOutputDirectory"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutputDirectory)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Decoder output parent does not exist: $outputParent"
}
$outputParentItem = Get-Item -LiteralPath $outputParent
if ($outputParentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
{
    throw 'Decoder output parent must not be a reparse point'
}

$packageManifestPath = Join-Path $resolvedPackageDirectory 'package-manifest.json'
[void](Assert-PBFileIdentity -Path $packageManifestPath -Expected $plan.deployment.packageManifest -Name 'local package manifest' -IgnoreExpectedPath)
$packageVerificationText = @(& (Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1') `
    -PackageDirectory $resolvedPackageDirectory -PackageSealPath $resolvedPackageSeal `
    -ExpectedManifestSha256 ([string]$plan.deployment.packageManifest.sha256) 2>&1)
$packageVerification = ConvertFrom-PBStrictJsonText -Text ($packageVerificationText -join "`n") -Name 'Decoder package verification output'
if ($packageVerification.verified -isnot [bool] -or -not [bool]$packageVerification.verified)
{
    throw 'Local Decoder package verifier did not return a verified result'
}
$decoderRelativePath = [string]$plan.applications.decoder.relativeExecutablePath
$decoderPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedPackageDirectory $decoderRelativePath.Replace('/', '\')))
if (-not $decoderPath.StartsWith($resolvedPackageDirectory + '\', [StringComparison]::OrdinalIgnoreCase))
{
    throw 'Decoder executable path escapes the verified package directory'
}
[void](Assert-PBFileIdentity -Path $decoderPath -Expected ([ordered]@{
    path = $decoderPath; size = $plan.applications.decoder.size; sha256 = $plan.applications.decoder.sha256
}) -Name 'local Decoder executable')
[void](Assert-PBFileIdentity -Path $resolvedMetadata -Expected $plan.deployment.remoteMetadata -Name 'local remote metadata' -IgnoreExpectedPath)
$metadata = Read-PBBoundedJson -Path $resolvedMetadata -MaximumBytes 64KB
if ([string]$metadata.runId -cne [string]$plan.runId -or [string]$metadata.channelType -cne 'RemoteVisual' -or
    [string]$metadata.remoteProvider -cne [string]$plan.remoteUi.visibleClaims.remoteProvider)
{
    throw 'Local remote metadata does not match the frozen pilot RunId/provider'
}

$resolvedReplayInput = ''
$replayInputIdentity = $null
if ($Mode -ceq 'Offline')
{
    $resolvedReplayInput = [System.IO.Path]::GetFullPath($ReplayInputPath)
    $replayInputIdentity = Get-PBFileIdentity -Path $resolvedReplayInput
    if ([UInt64]$replayInputIdentity.size -gt 16GB)
    {
        throw 'Offline Replay input exceeds the frozen 16 GiB reader cap'
    }
}

# Runtime reports persist absolute Replay and publish paths. Use the create-only final directory from the
# beginning so those paths remain valid. The process-result file is written last as the completion marker;
# an interrupted or failed directory is retained and cannot be silently reused.
New-Item -ItemType Directory -Path $resolvedOutputDirectory | Out-Null
$publishDirectory = Join-Path $resolvedOutputDirectory 'published'
New-Item -ItemType Directory -Path $publishDirectory | Out-Null
$namePrefix = if ($Mode -ceq 'Live') { 'live-decoder' } else { 'offline-decoder' }
$reportPath = Join-Path $resolvedOutputDirectory "$namePrefix-report.json"
$journalPath = Join-Path $resolvedOutputDirectory "$namePrefix-journal.jsonl"
$stdoutPath = Join-Path $resolvedOutputDirectory "$namePrefix.stdout.log"
$stderrPath = Join-Path $resolvedOutputDirectory "$namePrefix.stderr.log"
$processResultPath = Join-Path $resolvedOutputDirectory "$namePrefix-process-result.json"
$replayOutputPath = if ($Mode -ceq 'Live') { Join-Path $resolvedOutputDirectory 'live-capture.pbrv2' } else { '' }
$roi = $plan.monitorSafety.decoder.roiPhysicalRect
$monitorPreflightIdentity = $null
if ($isStep21Plan -and $Mode -ceq 'Live')
{
    $monitorPreflightPath = Join-Path $resolvedOutputDirectory 'live-decoder-monitor-preflight.json'
    [void](New-PBRemoteVisualMonitorPreflight -DecoderPath $decoderPath -Safety $plan.monitorSafety.decoder `
        -TargetRect $roi -EndpointRole DecoderLive -OutputPath $monitorPreflightPath)
    $monitorPreflightIdentity = Get-PBFileIdentity -Path $monitorPreflightPath -RelativeTo $resolvedOutputDirectory
}
if ($Mode -ceq 'Live')
{
    $arguments = @(
        '--headless-receive',
        '--output-dir', $publishDirectory,
        '--backend', [string]$plan.policy.captureBackend,
        '--profile', [string]$plan.profileToken,
        '--channel', 'remote',
        '--remote-provider', [string]$metadata.remoteProvider,
        '--remote-metadata', $resolvedMetadata,
        '--roi', [string]$roi.left, [string]$roi.top, [string]$roi.right, [string]$roi.bottom,
        '--timeout', [string]$plan.policy.decoderTimeoutSeconds,
        '--no-progress-seconds', [string]$plan.policy.noProgressSeconds,
        '--protected-monitor', [string]$plan.monitorSafety.decoder.protectedMonitorDeviceName,
        '--experiment-monitor', [string]$plan.monitorSafety.decoder.experimentMonitorDeviceName,
        '--replay-output', $replayOutputPath,
        '--replay-frames', [string]$plan.policy.replay.maximumFrames,
        '--replay-max-mib', [string]$plan.policy.replay.maximumMiB,
        '--replay-sample-fps', [string]$plan.policy.replay.maximumCaptureFramesPerSecond,
        '--run-id', [string]$plan.runId,
        '--journal', $journalPath,
        '--report', $reportPath
    )
}
else
{
    $arguments = @(
        '--headless-replay',
        '--replay-input', $resolvedReplayInput,
        '--output-dir', $publishDirectory,
        '--remote-provider', [string]$metadata.remoteProvider,
        '--remote-metadata', $resolvedMetadata,
        '--profile', [string]$plan.profileToken,
        '--timeout', [string]$plan.policy.offlineReplayTimeoutSeconds,
        '--no-progress-seconds', [string]$plan.policy.offlineNoProgressSeconds,
        '--replay-frames', [string]$plan.policy.replay.maximumFrames,
        '--replay-max-mib', [string]$plan.policy.replay.maximumMiB,
        '--run-id', [string]$plan.runId,
        '--journal', $journalPath,
        '--report', $reportPath
    )
}
$started = [DateTimeOffset]::UtcNow
$exitCode = -1
$failure = ''
if ($Mode -ceq 'Live')
{
    Write-Host ''
    Write-Host "Computer A live Decoder is ready. Start Computer B Encoder within $($plan.policy.decoderWarmupMaximumSeconds) seconds." -ForegroundColor Cyan
    Write-Host "The Decoder will only accept pixels captured by $expectedActualBackend from the frozen ExperimentMonitor ROI." -ForegroundColor Yellow
    Write-Host "RunId: $($plan.runId); logical FPS: $($plan.logicalFps); timeout/no-progress: $($plan.policy.decoderTimeoutSeconds)/$($plan.policy.noProgressSeconds) s"
    Write-Host ''
}
else
{
    Write-Host "Starting receiver-only offline reproduction from sealed live Replay: $resolvedReplayInput" -ForegroundColor Cyan
}
try
{
    & $decoderPath @arguments 1> $stdoutPath 2> $stderrPath
    $exitCode = $LASTEXITCODE
}
catch
{
    $failure = $_.Exception.Message
}
$ended = [DateTimeOffset]::UtcNow
$checks = [ordered]@{
    exitCodeZero = $exitCode -eq 0
    reportExists = Test-Path -LiteralPath $reportPath -PathType Leaf
    journalExists = Test-Path -LiteralPath $journalPath -PathType Leaf
    reportContract = $false
    journalEvidenceComplete = $false
    wholeFileDigestAndPublish = $false
    replayContract = $false
    publishedFileExists = $false
    reportArtifactPathsStable = $false
    replayObservationContract = $false
}
if ($isStep21Plan -and $Mode -ceq 'Live')
{
    $checks['monitorPreflight'] = $null -ne $monitorPreflightIdentity
}
$report = $null
$publishedIdentity = $null
$publishedPath = ''
$replayIdentity = $null
if ($checks.reportExists)
{
    try
    {
        $report = Read-PBBoundedJson -Path $reportPath -MaximumBytes 2MB
        $checks.journalEvidenceComplete = $report.evidence.journalEnabled -is [bool] -and [bool]$report.evidence.journalEnabled -and
            $report.evidence.valid -is [bool] -and [bool]$report.evidence.valid -and
            $report.evidence.journalTruncated -is [bool] -and -not [bool]$report.evidence.journalTruncated -and
            $report.evidence.journalFinished -is [bool] -and [bool]$report.evidence.journalFinished -and
            [UInt64]$report.evidence.journalSamples -gt 0
        $checks.reportContract = [string]$report.schema -ceq 'PixelBridge.RunReport.2' -and
            [string]$report.role -ceq 'Decoder' -and [string]$report.runId -ceq [string]$plan.runId -and
            [string]$report.state -ceq 'Completed' -and [string]$report.profile -ceq [string]$plan.profileName -and
            [UInt64]$report.visualProfileId -eq [UInt64]$plan.visualProfileId -and
            [UInt32]$report.visualLayoutVersion -eq [UInt32]$plan.visualLayoutVersion -and
            [UInt64]$report.originalFileBytes -eq [UInt64]$plan.source.size -and
            [UInt64]$report.verifiedRawBytes -eq [UInt64]$plan.source.size -and [UInt64]$report.remainingRawBytes -eq 0 -and
            [string]$report.errorDetail -ceq ''
        $falseAcceptedOk = $null -eq $report.falseAcceptedCodewords -or
            ($report.falseAcceptedCodewords -isnot [bool] -and [Int64]$report.falseAcceptedCodewords -eq 0)
        $checks.wholeFileDigestAndPublish = $report.wholeFileDigestVerified -is [bool] -and [bool]$report.wholeFileDigestVerified -and
            $report.finalPublishSucceeded -is [bool] -and [bool]$report.finalPublishSucceeded -and
            [string]$report.wholeFileDigest -cmatch '^[0-9a-f]{64}$' -and $falseAcceptedOk
        if ($Mode -ceq 'Live')
        {
            $checks.reportContract = [bool]$checks.reportContract -and
                $report.monitorSafety.preflightPassed -is [bool] -and [bool]$report.monitorSafety.preflightPassed -and
                [string]$report.monitorSafety.status -ceq 'PASS' -and [string]$report.actualBackend -ceq $expectedActualBackend
            $checks.replayContract = $report.replay.enabled -is [bool] -and [bool]$report.replay.enabled -and
                $report.replay.offlineMode -is [bool] -and -not [bool]$report.replay.offlineMode -and
                $report.replay.evidenceValid -is [bool] -and [bool]$report.replay.evidenceValid -and
                $report.replay.finalized -is [bool] -and [bool]$report.replay.finalized -and
                [UInt64]$report.replay.writtenFrames -gt 0 -and [UInt64]$report.replay.droppedFrames -eq 0 -and
                [UInt64]$report.replay.writtenDemodObservations -eq 0 -and
                [UInt64]$report.replay.droppedDemodObservations -eq 0 -and
                [UInt32]$report.replay.queueHighWater -le 2 -and
                [UInt32]$report.replay.maximumCaptureFramesPerSecond -eq [UInt32]$plan.policy.replay.maximumCaptureFramesPerSecond
            # Sampled production Replay intentionally records the selected captures but not live demod
            # observations: primary demod is never throttled to the evidence sampler. Offline truth is
            # therefore the complete Replay read plus an independent WholeFileDigest/publish result.
            $checks.replayObservationContract = [UInt64]$report.replay.writtenDemodObservations -eq 0 -and
                [UInt64]$report.replay.droppedDemodObservations -eq 0
        }
        else
        {
            $checks.reportContract = [bool]$checks.reportContract -and [string]$report.monitorSafety.status -ceq 'NotApplicableOfflineReplay'
            $checks.replayContract = $report.replay.enabled -is [bool] -and [bool]$report.replay.enabled -and
                $report.replay.offlineMode -is [bool] -and [bool]$report.replay.offlineMode -and
                $report.replay.evidenceValid -is [bool] -and [bool]$report.replay.evidenceValid -and
                $report.replay.finalized -is [bool] -and [bool]$report.replay.finalized -and
                [UInt64]$report.replay.offlineCaptureFrames -gt 0
            $checks.replayObservationContract = [UInt64]$report.replay.offlineDemodResults -eq [UInt64]$report.replay.offlineCaptureFrames -and
                [UInt64]$report.replay.offlineObservationComparisons -eq 0 -and
                [UInt64]$report.replay.offlineObservationMismatches -eq 0
        }
        if (-not [string]::IsNullOrWhiteSpace([string]$report.outputPath))
        {
            $publishedPath = [System.IO.Path]::GetFullPath([string]$report.outputPath)
            if ($publishedPath.StartsWith($publishDirectory + '\', [StringComparison]::OrdinalIgnoreCase) -and
                (Test-Path -LiteralPath $publishedPath -PathType Leaf))
            {
                $publishedIdentity = Get-PBFileIdentity -Path $publishedPath -RelativeTo $resolvedOutputDirectory
                $checks.publishedFileExists = [UInt64]$publishedIdentity.size -eq [UInt64]$plan.source.size
            }
        }
        if ($Mode -ceq 'Live' -and (Test-Path -LiteralPath $replayOutputPath -PathType Leaf))
        {
            $replayIdentity = Get-PBFileIdentity -Path $replayOutputPath -RelativeTo $resolvedOutputDirectory
            $checks.replayContract = [bool]$checks.replayContract -and [UInt64]$replayIdentity.size -eq [UInt64]$report.replay.fileBytes
        }
        $expectedReplayPath = if ($Mode -ceq 'Live') { $replayOutputPath } else { $resolvedReplayInput }
        $checks.reportArtifactPathsStable = $null -ne $publishedIdentity -and
            [System.IO.Path]::GetFullPath([string]$report.outputPath) -ieq [System.IO.Path]::GetFullPath($publishedPath) -and
            [System.IO.Path]::GetFullPath([string]$report.replay.path) -ieq [System.IO.Path]::GetFullPath($expectedReplayPath)
    }
    catch
    {
        if ([string]::IsNullOrEmpty($failure)) { $failure = $_.Exception.Message }
    }
}
$success = -not ($checks.Values -contains $false) -and [string]::IsNullOrEmpty($failure)
$processResult = [ordered]@{
    schema = 'PixelBridge.RemoteVisualPilotEndpointProcess.1'
    endpointRole = if ($Mode -ceq 'Live') { 'DecoderLive' } else { 'DecoderOffline' }
    runId = [string]$plan.runId
    planSha256 = [string]$frozenPlan.identity.sha256
    startedUtc = $started.ToString('o')
    endedUtc = $ended.ToString('o')
    startedUnixMilliseconds = $started.ToUnixTimeMilliseconds()
    endedUnixMilliseconds = $ended.ToUnixTimeMilliseconds()
    exitCode = $exitCode
    status = if ($success) { 'PASS' } else { 'FAIL' }
    failure = $failure
    toolHost = [ordered]@{
        powerShellEdition = [string]$PSVersionTable.PSEdition
        powerShellVersion = [string]$PSVersionTable.PSVersion
    }
    pixelOnlyContract = if ($Mode -ceq 'Live') { "Actual $expectedActualBackend pixels from the frozen ExperimentMonitor ROI only" } else { 'Receiver-only reproduction from the exact live Replay v2' }
    packageManifest = Get-PBFileIdentity -Path $packageManifestPath
    executable = Get-PBFileIdentity -Path $decoderPath
    remoteMetadata = Get-PBFileIdentity -Path $resolvedMetadata
    replayInput = $replayInputIdentity
    checks = $checks
    artifacts = [ordered]@{
        report = if ($checks.reportExists) { Get-PBFileIdentity -Path $reportPath -RelativeTo $resolvedOutputDirectory } else { $null }
        journal = if ($checks.journalExists) { Get-PBFileIdentity -Path $journalPath -RelativeTo $resolvedOutputDirectory } else { $null }
        published = $publishedIdentity
        replay = $replayIdentity
        stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) { Get-PBFileIdentity -Path $stdoutPath -RelativeTo $resolvedOutputDirectory } else { $null }
        stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) { Get-PBFileIdentity -Path $stderrPath -RelativeTo $resolvedOutputDirectory } else { $null }
    }
}
if ($isStep21Plan -and $Mode -ceq 'Live')
{
    $processResult.artifacts['monitorPreflight'] = $monitorPreflightIdentity
}
$evidenceReady = [DateTimeOffset]::UtcNow
$processResult['evidenceReadyUtc'] = $evidenceReady.ToString('o')
$processResult['evidenceReadyUnixMilliseconds'] = $evidenceReady.ToUnixTimeMilliseconds()
[void](Write-PBCreateOnlyJson -Path $processResultPath -Value $processResult -Depth 20)
if (-not $success)
{
    throw "Computer A $Mode Decoder pilot endpoint failed; evidence retained at $resolvedOutputDirectory"
}
if ($Mode -ceq 'Live')
{
    Write-Host ''
    Write-Host "DECODER_COMPLETED_AND_REPLAY_FINALIZED — wait $($plan.policy.postDecoderBroadcastProofSeconds) seconds, then stop Computer B Encoder (press Enter or Q in its console)." -ForegroundColor Green
    Write-Host "Live evidence: $resolvedOutputDirectory"
    Write-Host ''
}
else
{
    Write-Host "Offline Replay reproduction completed: $resolvedOutputDirectory" -ForegroundColor Green
}
$processResult | ConvertTo-Json -Depth 20

#Requires -Version 7.0

[CmdletBinding()]
param(
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
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [string]$RemoteMetadataPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
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
$encoderRuntimeMonitorSafety = [string]$plan.profileToken -ceq 'remote-lf4'
$resolvedPackageDirectory = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedSource = [System.IO.Path]::GetFullPath($SourcePath)
$resolvedMetadata = [System.IO.Path]::GetFullPath($RemoteMetadataPath)
$resolvedOutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
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
    throw "Create-only Encoder output directory or partial already exists: $resolvedOutputDirectory"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutputDirectory)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Encoder output parent does not exist: $outputParent"
}
$outputParentItem = Get-Item -LiteralPath $outputParent
if ($outputParentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
{
    throw 'Encoder output parent must not be a reparse point'
}

$packageManifestPath = Join-Path $resolvedPackageDirectory 'package-manifest.json'
[void](Assert-PBFileIdentity -Path $packageManifestPath -Expected $plan.deployment.packageManifest -Name 'local package manifest' -IgnoreExpectedPath)
$packageVerificationText = @(& (Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1') `
    -PackageDirectory $resolvedPackageDirectory -PackageSealPath $resolvedPackageSeal `
    -ExpectedManifestSha256 ([string]$plan.deployment.packageManifest.sha256) 2>&1)
$packageVerification = ConvertFrom-PBStrictJsonText -Text ($packageVerificationText -join "`n") -Name 'Encoder package verification output'
if ($packageVerification.verified -isnot [bool] -or -not [bool]$packageVerification.verified)
{
    throw 'Local Encoder package verifier did not return a verified result'
}

$encoderRelativePath = [string]$plan.applications.encoder.relativeExecutablePath
$encoderPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedPackageDirectory $encoderRelativePath.Replace('/', '\')))
if (-not $encoderPath.StartsWith($resolvedPackageDirectory + '\', [StringComparison]::OrdinalIgnoreCase))
{
    throw 'Encoder executable path escapes the verified package directory'
}
[void](Assert-PBFileIdentity -Path $encoderPath -Expected ([ordered]@{
    path = $encoderPath; size = $plan.applications.encoder.size; sha256 = $plan.applications.encoder.sha256
}) -Name 'local Encoder executable')
$monitorProbeDecoderPath = ''
if ($isStep21Plan)
{
    $decoderRelativePath = [string]$plan.applications.decoder.relativeExecutablePath
    $monitorProbeDecoderPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedPackageDirectory $decoderRelativePath.Replace('/', '\')))
    if (-not $monitorProbeDecoderPath.StartsWith($resolvedPackageDirectory + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Monitor preflight Decoder path escapes the verified package directory'
    }
    [void](Assert-PBFileIdentity -Path $monitorProbeDecoderPath -Expected ([ordered]@{
        path = $monitorProbeDecoderPath; size = $plan.applications.decoder.size; sha256 = $plan.applications.decoder.sha256
    }) -Name 'local monitor preflight Decoder executable')
}
[void](Assert-PBFileIdentity -Path $resolvedMetadata -Expected $plan.deployment.remoteMetadata -Name 'local remote metadata' -IgnoreExpectedPath)
$metadata = Read-PBBoundedJson -Path $resolvedMetadata -MaximumBytes 64KB
if ([string]$metadata.runId -cne [string]$plan.runId -or [string]$metadata.channelType -cne 'RemoteVisual' -or
    [string]$metadata.remoteProvider -cne [string]$plan.remoteUi.visibleClaims.remoteProvider)
{
    throw 'Local remote metadata does not match the frozen pilot RunId/provider'
}
$sourceIdentity = Get-PBFileIdentity -Path $resolvedSource
if ([UInt64]$sourceIdentity.size -ne [UInt64]$plan.source.size -or [string]$sourceIdentity.sha256 -cne [string]$plan.source.sha256)
{
    throw 'Local Computer B source differs from the frozen 1 MiB source identity'
}

# Runtime reports persist absolute source/artifact paths. Use the create-only final directory from the
# beginning so those paths remain authoritative after the endpoint exits. The process-result file is
# written last and is the completion marker; an interrupted or failed directory is intentionally retained.
New-Item -ItemType Directory -Path $resolvedOutputDirectory | Out-Null
$reportPath = Join-Path $resolvedOutputDirectory 'encoder-report.json'
$journalPath = Join-Path $resolvedOutputDirectory 'encoder-journal.jsonl'
$stdoutPath = Join-Path $resolvedOutputDirectory 'encoder.stdout.log'
$stderrPath = Join-Path $resolvedOutputDirectory 'encoder.stderr.log'
$processResultPath = Join-Path $resolvedOutputDirectory 'encoder-process-result.json'
$dataWindow = $plan.monitorSafety.encoder.dataWindowPhysicalRect
$monitorPreflightIdentity = $null
if ($isStep21Plan)
{
    $monitorPreflightPath = Join-Path $resolvedOutputDirectory 'encoder-monitor-preflight.json'
    [void](New-PBRemoteVisualMonitorPreflight -DecoderPath $monitorProbeDecoderPath `
        -Safety $plan.monitorSafety.encoder -TargetRect $dataWindow -EndpointRole Encoder `
        -OutputPath $monitorPreflightPath)
    $monitorPreflightIdentity = Get-PBFileIdentity -Path $monitorPreflightPath -RelativeTo $resolvedOutputDirectory
}
$arguments = @(
    '--headless-broadcast',
    '--source', $resolvedSource,
    '--profile', [string]$plan.profileToken,
    '--channel', 'remote',
    '--remote-provider', [string]$metadata.remoteProvider,
    '--remote-metadata', $resolvedMetadata,
    '--compression', 'off',
    '--origin', [string]$dataWindow.left, [string]$dataWindow.top,
    '--seconds', [string]$plan.policy.encoderHardMaximumSeconds,
    '--logical-fps', [string]$plan.logicalFps,
    '--control-repetitions', [string]$plan.policy.controlRepetitions,
    '--run-id', [string]$plan.runId,
    '--manual-stop',
    '--journal', $journalPath,
    '--report', $reportPath
)
if ($encoderRuntimeMonitorSafety)
{
    $arguments += @('--protected-monitor', [string]$plan.monitorSafety.encoder.protectedMonitorDeviceName,
        '--experiment-monitor', [string]$plan.monitorSafety.encoder.experimentMonitorDeviceName)
}
$started = [DateTimeOffset]::UtcNow
$exitCode = -1
$failure = ''
Write-Host ''
Write-Host "Computer B Encoder is about to broadcast the frozen $($plan.profileToken) RemoteVisual field run." -ForegroundColor Cyan
Write-Host 'Do not use file transfer, clipboard, shared folders, or any non-visual payload path.' -ForegroundColor Yellow
Write-Host "After Computer A reports Completed + Replay finalized, wait $($plan.policy.postDecoderBroadcastProofSeconds) seconds, then press Enter or Q in THIS console." -ForegroundColor Green
Write-Host "Hard maximum: $($plan.policy.encoderHardMaximumSeconds) s; logical FPS: $($plan.logicalFps); RunId: $($plan.runId)"
Write-Host ''
try
{
    & $encoderPath @arguments 1> $stdoutPath 2> $stderrPath
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
    manualStopConfirmed = $false
    reportContract = $false
    journalEvidenceComplete = $false
}
if ($isStep21Plan)
{
    $checks['monitorPreflight'] = $null -ne $monitorPreflightIdentity
}
$report = $null
if ($checks.reportExists)
{
    try
    {
        $report = Read-PBBoundedJson -Path $reportPath -MaximumBytes 2MB
        $checks.manualStopConfirmed = [string]$report.state -ceq 'Stopped' -and [string]$report.statusMessage -ceq 'Broadcast stopped by user; no sender-side receiver completion was inferred'
        $checks.journalEvidenceComplete = $report.evidence.journalEnabled -is [bool] -and [bool]$report.evidence.journalEnabled -and
            $report.evidence.valid -is [bool] -and [bool]$report.evidence.valid -and
            $report.evidence.journalTruncated -is [bool] -and -not [bool]$report.evidence.journalTruncated -and
            $report.evidence.journalFinished -is [bool] -and [bool]$report.evidence.journalFinished -and
            [UInt64]$report.evidence.journalSamples -gt 0
        $monitorSafetyReportOk = if ($encoderRuntimeMonitorSafety)
        {
            $report.monitorSafety.preflightPassed -is [bool] -and [bool]$report.monitorSafety.preflightPassed -and
                [string]$report.monitorSafety.status -ceq 'PASS'
        }
        else
        {
            $report.monitorSafety.preflightPassed -is [bool] -and -not [bool]$report.monitorSafety.preflightPassed -and
                [string]$report.monitorSafety.status -ceq 'NotRequired'
        }
        $checks.reportContract = [string]$report.schema -ceq 'PixelBridge.RunReport.2' -and
            [string]$report.role -ceq 'Encoder' -and [string]$report.runId -ceq [string]$plan.runId -and
            [string]$report.profile -ceq [string]$plan.profileName -and
            [UInt64]$report.visualProfileId -eq [UInt64]$plan.visualProfileId -and
            [UInt32]$report.visualLayoutVersion -eq [UInt32]$plan.visualLayoutVersion -and
            [UInt64]$report.fileBytes -eq [UInt64]$plan.source.size -and
            [UInt32]$report.configuredLogicalVisualFps -eq [UInt32]$plan.logicalFps -and
            [UInt32]$report.configuredControlRepetitions -eq [UInt32]$plan.policy.controlRepetitions -and
            $report.sourceStable -is [bool] -and [bool]$report.sourceStable -and
            [System.IO.Path]::GetFullPath([string]$report.sourcePath) -ieq $resolvedSource -and
            $monitorSafetyReportOk -and [string]$report.errorDetail -ceq ''
    }
    catch
    {
        if ([string]::IsNullOrEmpty($failure)) { $failure = $_.Exception.Message }
    }
}
$success = -not ($checks.Values -contains $false) -and [string]::IsNullOrEmpty($failure)
$processResult = [ordered]@{
    schema = 'PixelBridge.RemoteVisualPilotEndpointProcess.1'
    endpointRole = 'Encoder'
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
    manualStopContract = "Wait $($plan.policy.postDecoderBroadcastProofSeconds) seconds after Computer A reports Completed and Replay finalized, then press Enter or Q; --seconds is a hard maximum"
    noSideChannelContract = 'No file transfer, clipboard, shared folder, IPC, or payload path other than rendered/captured pixels'
    packageManifest = Get-PBFileIdentity -Path $packageManifestPath
    executable = Get-PBFileIdentity -Path $encoderPath
    source = $sourceIdentity
    remoteMetadata = Get-PBFileIdentity -Path $resolvedMetadata
    checks = $checks
    artifacts = [ordered]@{
        report = if ($checks.reportExists) { Get-PBFileIdentity -Path $reportPath -RelativeTo $resolvedOutputDirectory } else { $null }
        journal = if ($checks.journalExists) { Get-PBFileIdentity -Path $journalPath -RelativeTo $resolvedOutputDirectory } else { $null }
        stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) { Get-PBFileIdentity -Path $stdoutPath -RelativeTo $resolvedOutputDirectory } else { $null }
        stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) { Get-PBFileIdentity -Path $stderrPath -RelativeTo $resolvedOutputDirectory } else { $null }
    }
}
if ($isStep21Plan)
{
    $processResult.artifacts['monitorPreflight'] = $monitorPreflightIdentity
}
$evidenceReady = [DateTimeOffset]::UtcNow
$processResult['evidenceReadyUtc'] = $evidenceReady.ToString('o')
$processResult['evidenceReadyUnixMilliseconds'] = $evidenceReady.ToUnixTimeMilliseconds()
[void](Write-PBCreateOnlyJson -Path $processResultPath -Value $processResult -Depth 20)
if (-not $success)
{
    throw "Computer B Encoder pilot endpoint failed; evidence retained at $resolvedOutputDirectory"
}
Write-Host "Computer B Encoder stopped manually and evidence is complete: $resolvedOutputDirectory" -ForegroundColor Green
$processResult | ConvertTo-Json -Depth 20

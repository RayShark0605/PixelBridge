#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PlanPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPlanSha256,

    [Parameter(Mandatory = $true)]
    [string]$EncoderEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$LiveDecoderEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OfflineDecoderEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

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
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

function Get-EvidenceArtifact
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object]$Identity,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$AllowEmpty
    )
    if ($Identity -isnot [System.Collections.IDictionary] -or
        [string]::IsNullOrWhiteSpace([string]$Identity.path) -or $Identity.size -is [bool] -or
        (-not $AllowEmpty -and [UInt64]$Identity.size -eq 0) -or [string]$Identity.sha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "$Name does not contain a valid relative path/size/SHA-256 identity"
    }
    if ([System.IO.Path]::IsPathRooted([string]$Identity.path) -or [string]$Identity.path -match '(^|/|\\)\.\.($|/|\\)')
    {
        throw "$Name must use a confined relative evidence path"
    }
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    $resolvedPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedRoot ([string]$Identity.path).Replace('/', '\')))
    if (-not $resolvedPath.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        throw "$Name escapes its evidence directory"
    }
    $actual = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$actual.size -ne [UInt64]$Identity.size -or [string]$actual.sha256 -cne [string]$Identity.sha256)
    {
        throw "$Name exact identity mismatch"
    }
    return $resolvedPath
}

function Assert-ExactDictionaryKeys
{
    param(
        [Parameter(Mandatory = $true)][object]$Dictionary,
        [Parameter(Mandatory = $true)][string[]]$ExpectedKeys,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Dictionary -isnot [System.Collections.IDictionary])
    {
        throw "$Name must be a JSON object"
    }
    $actualKeys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($key in $Dictionary.Keys)
    {
        [void]$actualKeys.Add([string]$key)
    }
    if ($actualKeys.Count -ne $ExpectedKeys.Count)
    {
        throw "$Name does not contain the exact required key set"
    }
    foreach ($key in $ExpectedKeys)
    {
        if (-not $actualKeys.Remove($key))
        {
            throw "$Name is missing exact key '$key'"
        }
    }
}

function Assert-ExactTimestampPair
{
    param(
        [Parameter(Mandatory = $true)][object]$UtcValue,
        [Parameter(Mandatory = $true)][object]$UnixMillisecondsValue,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($UnixMillisecondsValue -is [bool] -or $UnixMillisecondsValue -isnot [Int64] -or [Int64]$UnixMillisecondsValue -le 0)
    {
        throw "$Name Unix-millisecond value is not a positive Int64"
    }
    try
    {
        $parsed = [DateTimeOffset]::ParseExact([string]$UtcValue, 'o', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    }
    catch
    {
        throw "$Name UTC value is not an exact round-trip timestamp"
    }
    if ($parsed.Offset -ne [TimeSpan]::Zero -or $parsed.ToUnixTimeMilliseconds() -ne [Int64]$UnixMillisecondsValue)
    {
        throw "$Name UTC and Unix-millisecond values disagree"
    }
}

function Assert-IdentityMatches
{
    param(
        [Parameter(Mandatory = $true)][object]$Actual,
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )
    Assert-PBIdentityShape -Identity $Actual -Name $Name
    if ($Expected -isnot [System.Collections.IDictionary] -or $Expected.size -is [bool] -or
        [UInt64]$Expected.size -eq 0 -or [string]$Expected.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        [UInt64]$Actual.size -ne [UInt64]$Expected.size -or [string]$Actual.sha256 -cne [string]$Expected.sha256)
    {
        throw "$Name identity differs from the frozen pilot plan"
    }
}

function Read-EndpointProcess
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string]$ExpectedRole,
        [Parameter(Mandatory = $true)][object]$Plan
    )
    $path = Join-Path $Root $FileName
    $process = Read-PBBoundedJson -Path $path -MaximumBytes 2MB
    if ([string]$process.schema -cne 'PixelBridge.RemoteVisualPilotEndpointProcess.1' -or
        [string]$process.endpointRole -cne $ExpectedRole -or [string]$process.runId -cne [string]$Plan.value.runId -or
        [string]$process.planSha256 -cne [string]$Plan.identity.sha256 -or [string]$process.status -cne 'PASS' -or
        [Int32]$process.exitCode -ne 0 -or -not [string]::IsNullOrEmpty([string]$process.failure))
    {
        throw "$ExpectedRole endpoint process result is not a successful frozen-plan execution"
    }
    $expectedCheckKeys = if ($ExpectedRole -ceq 'Encoder')
    {
        @('exitCodeZero', 'reportExists', 'journalExists', 'manualStopConfirmed', 'reportContract', 'journalEvidenceComplete')
    }
    else
    {
        @('exitCodeZero', 'reportExists', 'journalExists', 'reportContract', 'journalEvidenceComplete',
            'wholeFileDigestAndPublish', 'replayContract', 'publishedFileExists', 'reportArtifactPathsStable', 'replayObservationContract')
    }
    if ([string]$Plan.value.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2' -and
        $ExpectedRole -in @('Encoder', 'DecoderLive'))
    {
        $expectedCheckKeys += 'monitorPreflight'
    }
    $expectedArtifactKeys = if ($ExpectedRole -ceq 'Encoder')
    {
        @('report', 'journal', 'stdout', 'stderr')
    }
    else
    {
        @('report', 'journal', 'published', 'replay', 'stdout', 'stderr')
    }
    if ([string]$Plan.value.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2' -and
        $ExpectedRole -in @('Encoder', 'DecoderLive'))
    {
        $expectedArtifactKeys += 'monitorPreflight'
    }
    Assert-ExactDictionaryKeys -Dictionary $process.checks -ExpectedKeys $expectedCheckKeys -Name "$ExpectedRole endpoint checks"
    Assert-ExactDictionaryKeys -Dictionary $process.artifacts -ExpectedKeys $expectedArtifactKeys -Name "$ExpectedRole endpoint artifacts"
    Assert-ExactDictionaryKeys -Dictionary $process.toolHost -ExpectedKeys @('powerShellEdition', 'powerShellVersion') -Name "$ExpectedRole tool host"
    Assert-ExactTimestampPair -UtcValue $process.startedUtc -UnixMillisecondsValue $process.startedUnixMilliseconds -Name "$ExpectedRole process start"
    Assert-ExactTimestampPair -UtcValue $process.endedUtc -UnixMillisecondsValue $process.endedUnixMilliseconds -Name "$ExpectedRole process end"
    Assert-ExactTimestampPair -UtcValue $process.evidenceReadyUtc -UnixMillisecondsValue $process.evidenceReadyUnixMilliseconds -Name "$ExpectedRole evidence-ready"
    $toolHostVersion = $null
    try
    {
        $toolHostVersion = [Version][string]$process.toolHost.powerShellVersion
    }
    catch
    {
        throw "$ExpectedRole endpoint process result has an invalid PowerShell tool-host version"
    }
    if ($process.toolHost -isnot [System.Collections.IDictionary] -or
        [string]$process.toolHost.powerShellEdition -cne 'Core' -or $toolHostVersion.Major -lt 7)
    {
        throw "$ExpectedRole endpoint process result was not produced by the required PowerShell 7 tool host"
    }
    foreach ($check in $process.checks.GetEnumerator())
    {
        if ($check.Value -isnot [bool] -or -not [bool]$check.Value)
        {
            throw "$ExpectedRole endpoint process check failed: $($check.Key)"
        }
    }
    Assert-IdentityMatches -Actual $process.packageManifest -Expected $Plan.value.deployment.packageManifest -Name "$ExpectedRole package manifest"
    Assert-IdentityMatches -Actual $process.remoteMetadata -Expected $Plan.value.deployment.remoteMetadata -Name "$ExpectedRole remote metadata"
    $applicationKey = if ($ExpectedRole -ceq 'Encoder') { 'encoder' } else { 'decoder' }
    Assert-IdentityMatches -Actual $process.executable -Expected $Plan.value.applications[$applicationKey] -Name "$ExpectedRole executable"
    if ($ExpectedRole -ceq 'Encoder')
    {
        Assert-IdentityMatches -Actual $process.source -Expected $Plan.value.source -Name 'Encoder source'
    }
    $reportPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.report -Name "$ExpectedRole report"
    $journalPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.journal -Name "$ExpectedRole journal"
    $stdoutPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.stdout -Name "$ExpectedRole stdout" -AllowEmpty
    $stderrPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.stderr -Name "$ExpectedRole stderr" -AllowEmpty
    $publishedPath = $null
    if ($null -ne $process.artifacts.published)
    {
        $publishedPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.published -Name "$ExpectedRole published file"
    }
    $replayPath = $null
    if ($null -ne $process.artifacts.replay)
    {
        $replayPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.replay -Name "$ExpectedRole Replay"
    }
    $monitorPreflightPath = $null
    if ($process.artifacts.Contains('monitorPreflight') -and $null -ne $process.artifacts.monitorPreflight)
    {
        $monitorPreflightPath = Get-EvidenceArtifact -Root $Root -Identity $process.artifacts.monitorPreflight `
            -Name "$ExpectedRole monitor preflight"
        $monitorPreflight = Read-PBBoundedJson -Path $monitorPreflightPath -MaximumBytes 2MB
        if ([string]$monitorPreflight.schema -cne 'PixelBridge.RemoteVisualMonitorPreflight.1' -or
            [string]$monitorPreflight.endpointRole -cne $ExpectedRole -or [string]$monitorPreflight.status -cne 'PASS')
        {
            throw "$ExpectedRole monitor preflight artifact is invalid"
        }
    }
    return [ordered]@{
        root = $Root
        processPath = $path
        process = $process
        reportPath = $reportPath
        journalPath = $journalPath
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        publishedPath = $publishedPath
        replayPath = $replayPath
        monitorPreflightPath = $monitorPreflightPath
        report = Read-PBBoundedJson -Path $reportPath -MaximumBytes 2MB
    }
}

function Read-BoundedJournal
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedRole,
        [Parameter(Mandatory = $true)][string]$RunId
    )
    $item = Get-Item -LiteralPath $Path
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        [UInt64]$item.Length -eq 0 -or [UInt64]$item.Length -gt 64MB)
    {
        throw "$ExpectedRole journal is empty, oversized, or a reparse point"
    }
    $entries = [Collections.Generic.List[object]]::new()
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try
    {
        $reader = [System.IO.StreamReader]::new($stream, [System.Text.UTF8Encoding]::new($false, $true), $true, 65536, $true)
        try
        {
            $previousUnixMs = [Int64]::MinValue
            while (-not $reader.EndOfStream)
            {
                $line = $reader.ReadLine()
                if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -gt 65536 -or $entries.Count -ge 100000)
                {
                    throw "$ExpectedRole journal contains an empty/oversized line or too many records"
                }
                $entry = ConvertFrom-PBStrictJsonText -Text $line -Name "$ExpectedRole journal line"
                if ([string]$entry.schema -cne 'PixelBridge.RunJournal.1' -or [string]$entry.role -cne $ExpectedRole -or
                    [string]$entry.runId -cne $RunId -or $entry.unixMs -is [bool] -or $entry.unixMs -isnot [Int64] -or
                    [Int64]$entry.unixMs -le 0 -or [Int64]$entry.unixMs -lt $previousUnixMs)
                {
                    throw "$ExpectedRole journal schema/identity/time monotonicity mismatch"
                }
                $previousUnixMs = [Int64]$entry.unixMs
                [void]$entries.Add($entry)
            }
        }
        finally
        {
            $reader.Dispose()
        }
    }
    finally
    {
        $stream.Dispose()
    }
    if ($entries.Count -eq 0)
    {
        throw "$ExpectedRole journal contains no records"
    }
    return $entries
}

function Assert-DecoderReport
{
    param(
        [Parameter(Mandatory = $true)][object]$Report,
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][bool]$Offline
    )
    if ([string]$Report.schema -cne 'PixelBridge.RunReport.2' -or [string]$Report.role -cne 'Decoder' -or
        [string]$Report.runId -cne [string]$Plan.runId -or [string]$Report.state -cne 'Completed' -or
        [string]$Report.profile -cne [string]$Plan.profileName -or [UInt64]$Report.visualProfileId -ne [UInt64]$Plan.visualProfileId -or
        [UInt32]$Report.visualLayoutVersion -ne [UInt32]$Plan.visualLayoutVersion -or
        [UInt64]$Report.originalFileBytes -ne [UInt64]$Plan.source.size -or
        [UInt64]$Report.verifiedRawBytes -ne [UInt64]$Plan.source.size -or [UInt64]$Report.remainingRawBytes -ne 0 -or
        $Report.wholeFileDigestVerified -isnot [bool] -or -not [bool]$Report.wholeFileDigestVerified -or
        $Report.finalPublishSucceeded -isnot [bool] -or -not [bool]$Report.finalPublishSucceeded -or
        [string]$Report.wholeFileDigest -cnotmatch '^[0-9a-f]{64}$' -or [string]$Report.errorDetail -cne '' -or
        [UInt64]$Report.outerAdmission.conflictRejections -ne 0)
    {
        throw "Decoder report authoritative recovery contract failed (offline=$Offline)"
    }
    if ($null -ne $Report.falseAcceptedCodewords -and
        ($Report.falseAcceptedCodewords -is [bool] -or [Int64]$Report.falseAcceptedCodewords -ne 0))
    {
        throw "Decoder report contains false accepted codewords (offline=$Offline)"
    }
    if ($Report.replay.enabled -isnot [bool] -or -not [bool]$Report.replay.enabled -or
        $Report.replay.evidenceValid -isnot [bool] -or -not [bool]$Report.replay.evidenceValid -or
        $Report.replay.finalized -isnot [bool] -or -not [bool]$Report.replay.finalized -or
        [bool]$Report.replay.offlineMode -ne $Offline)
    {
        throw "Decoder Replay contract failed (offline=$Offline)"
    }
    if (-not $Offline)
    {
        $expectedBackend = if ([string]$Plan.policy.captureBackend -ceq 'wgc') { 'WGC' } else { 'DXGI' }
        if ([string]$Report.actualBackend -cne $expectedBackend -or $Report.monitorSafety.preflightPassed -isnot [bool] -or
            -not [bool]$Report.monitorSafety.preflightPassed -or [string]$Report.monitorSafety.status -cne 'PASS' -or
            [UInt64]$Report.replay.writtenFrames -eq 0 -or [UInt64]$Report.replay.droppedFrames -ne 0 -or
            [UInt64]$Report.replay.writtenDemodObservations -ne 0 -or
            [UInt64]$Report.replay.droppedDemodObservations -ne 0 -or [UInt32]$Report.replay.queueHighWater -gt 2 -or
            [UInt32]$Report.replay.maximumCaptureFramesPerSecond -ne [UInt32]$Plan.policy.replay.maximumCaptureFramesPerSecond)
        {
            throw 'Live Decoder backend/monitor/Replay sampling contract failed'
        }
    }
    elseif ([string]$Report.monitorSafety.status -cne 'NotApplicableOfflineReplay' -or
        [UInt64]$Report.replay.offlineCaptureFrames -eq 0 -or
        [UInt64]$Report.replay.offlineDemodResults -ne [UInt64]$Report.replay.offlineCaptureFrames -or
        [UInt64]$Report.replay.offlineObservationComparisons -ne 0 -or
        [UInt64]$Report.replay.offlineObservationMismatches -ne 0)
    {
        throw 'Offline Decoder Replay reproduction contract failed'
    }
    if ([UInt32]$Report.frameLeaseHighWater -gt 4 -or [UInt32]$Report.demodPendingHighWater -gt 8 -or
        [UInt32]$Report.resultQueueHighWater -gt 8)
    {
        throw "Decoder report exceeds the bounded production pipeline queues (offline=$Offline)"
    }
}

function Assert-EndpointTimeline
{
    param(
        [Parameter(Mandatory = $true)][object]$Endpoint,
        [Parameter(Mandatory = $true)][object]$Report,
        [Parameter(Mandatory = $true)][string]$Name
    )
    foreach ($value in @($Endpoint.process.startedUnixMilliseconds, $Endpoint.process.endedUnixMilliseconds,
        $Endpoint.process.evidenceReadyUnixMilliseconds,
        $Report.runStartedUnixMilliseconds, $Report.runEndedUnixMilliseconds))
    {
        if ($null -eq $value -or $value -is [bool] -or [Int64]$value -le 0)
        {
            throw "$Name contains a missing or invalid authoritative timestamp"
        }
    }
    $processStart = [Int64]$Endpoint.process.startedUnixMilliseconds
    $processEnd = [Int64]$Endpoint.process.endedUnixMilliseconds
    $evidenceReady = [Int64]$Endpoint.process.evidenceReadyUnixMilliseconds
    $reportStart = [Int64]$Report.runStartedUnixMilliseconds
    $reportEnd = [Int64]$Report.runEndedUnixMilliseconds
    if ($processStart -gt $reportStart -or $reportStart -gt $reportEnd -or $reportEnd -gt $processEnd -or
        $processEnd -gt $evidenceReady)
    {
        throw "$Name report timestamps escape the endpoint process interval"
    }
}

function Assert-EndpointArtifactInventory
{
    param(
        [Parameter(Mandatory = $true)][object]$Endpoint,
        [Parameter(Mandatory = $true)][string]$ExpectedRole
    )
    $root = [System.IO.Path]::GetFullPath([string]$Endpoint.root).TrimEnd('\')
    $expectedRootPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @($Endpoint.processPath, $Endpoint.reportPath, $Endpoint.journalPath, $Endpoint.stdoutPath, $Endpoint.stderrPath))
    {
        $resolvedPath = [System.IO.Path]::GetFullPath([string]$path)
        if ([System.IO.Path]::GetDirectoryName($resolvedPath) -ine $root)
        {
            throw "$ExpectedRole endpoint root artifact is not directly below its evidence root: $resolvedPath"
        }
        [void]$expectedRootPaths.Add($resolvedPath)
    }
    if ($null -ne $Endpoint.monitorPreflightPath)
    {
        $resolvedPreflightPath = [System.IO.Path]::GetFullPath([string]$Endpoint.monitorPreflightPath)
        if ([System.IO.Path]::GetDirectoryName($resolvedPreflightPath) -ine $root)
        {
            throw "$ExpectedRole monitor preflight is not directly below its evidence root"
        }
        [void]$expectedRootPaths.Add($resolvedPreflightPath)
    }
    if ($null -ne $Endpoint.replayPath)
    {
        $resolvedReplayPath = [System.IO.Path]::GetFullPath([string]$Endpoint.replayPath)
        if ([System.IO.Path]::GetDirectoryName($resolvedReplayPath) -ine $root)
        {
            throw "$ExpectedRole Replay is not directly below its evidence root"
        }
        [void]$expectedRootPaths.Add($resolvedReplayPath)
    }
    $rootItems = @(Get-ChildItem -LiteralPath $root -Force)
    if ($rootItems.Count -gt 16)
    {
        throw "$ExpectedRole endpoint evidence root exceeds the bounded flat inventory"
    }
    $actualRootPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $directories = [Collections.Generic.List[object]]::new()
    foreach ($item in $rootItems)
    {
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
        {
            throw "$ExpectedRole endpoint inventory contains a reparse point: $($item.FullName)"
        }
        if ($item.PSIsContainer)
        {
            [void]$directories.Add($item)
        }
        else
        {
            [void]$actualRootPaths.Add([System.IO.Path]::GetFullPath($item.FullName))
        }
    }
    if ($actualRootPaths.Count -ne $expectedRootPaths.Count)
    {
        throw "$ExpectedRole endpoint root file inventory differs from process-result"
    }
    foreach ($path in $expectedRootPaths)
    {
        if (-not $actualRootPaths.Contains($path))
        {
            throw "$ExpectedRole endpoint root file inventory is missing $path"
        }
    }
    if ($ExpectedRole -ceq 'Encoder')
    {
        if ($directories.Count -ne 0 -or $null -ne $Endpoint.publishedPath)
        {
            throw 'Encoder endpoint evidence unexpectedly contains a published directory or file'
        }
        return
    }
    if ($directories.Count -ne 1 -or $directories[0].Name -cne 'published' -or $null -eq $Endpoint.publishedPath)
    {
        throw "$ExpectedRole endpoint evidence does not contain exactly one published directory"
    }
    $publishedItems = @(Get-ChildItem -LiteralPath $directories[0].FullName -Force)
    if ($publishedItems.Count -ne 1 -or $publishedItems[0].PSIsContainer -or
        ($publishedItems[0].Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        [System.IO.Path]::GetFullPath($publishedItems[0].FullName) -ine [System.IO.Path]::GetFullPath([string]$Endpoint.publishedPath))
    {
        throw "$ExpectedRole published directory inventory differs from process-result"
    }
}

function Assert-JournalTimeline
{
    param(
        [Parameter(Mandatory = $true)][object]$Endpoint,
        [Parameter(Mandatory = $true)][object]$Entries,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $processStart = [Int64]$Endpoint.process.startedUnixMilliseconds
    $processEnd = [Int64]$Endpoint.process.endedUnixMilliseconds
    if ([Int64]$Entries[0].unixMs -lt $processStart -or [Int64]$Entries[$Entries.Count - 1].unixMs -gt $processEnd)
    {
        throw "$Name journal timestamps escape the endpoint process interval"
    }
}

$frozenPlan = Import-PBRemoteVisualPilotPlan -Path $PlanPath -ExpectedSha256 $ExpectedPlanSha256
$plan = $frozenPlan.value
$resolvedEncoderRoot = [System.IO.Path]::GetFullPath($EncoderEvidenceDirectory)
$resolvedLiveRoot = [System.IO.Path]::GetFullPath($LiveDecoderEvidenceDirectory)
$resolvedOfflineRoot = [System.IO.Path]::GetFullPath($OfflineDecoderEvidenceDirectory)
$resolvedSource = [System.IO.Path]::GetFullPath($SourcePath)
$resolvedPython = [System.IO.Path]::GetFullPath($PythonPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
foreach ($root in @($resolvedEncoderRoot, $resolvedLiveRoot, $resolvedOfflineRoot))
{
    if (-not (Test-Path -LiteralPath $root -PathType Container) -or
        (Get-Item -LiteralPath $root).Attributes -band [System.IO.FileAttributes]::ReparsePoint)
    {
        throw "Evidence root does not exist or is a reparse point: $root"
    }
}
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath "$resolvedOutput.partial"))
{
    throw "Create-only pilot verification output or partial already exists: $resolvedOutput"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Pilot verification output parent does not exist: $outputParent"
}
$outputParentItem = Get-Item -LiteralPath $outputParent
if ($outputParentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
{
    throw 'Pilot verification output parent must not be a reparse point'
}
$sourceIdentity = Get-PBFileIdentity -Path $resolvedSource
if ([UInt64]$sourceIdentity.size -ne [UInt64]$plan.source.size -or [string]$sourceIdentity.sha256 -cne [string]$plan.source.sha256)
{
    throw 'External source identity differs from the frozen 1 MiB pilot source'
}

$packageManifestPath = [System.IO.Path]::GetFullPath([string]$plan.deployment.packageManifest.path)
[void](Assert-PBFileIdentity -Path $packageManifestPath -Expected $plan.deployment.packageManifest -Name 'package manifest')
$packageManifest = Read-PBBoundedJson -Path $packageManifestPath -MaximumBytes 4MB
if ([string]$packageManifest.schema -cne 'PixelBridge.PortablePackage.2' -or
    [string]$packageManifest.buildIdentity.headCommit -cnotmatch '^[0-9a-f]{40}$')
{
    throw 'Frozen package manifest schema or build commit identity is invalid'
}

$deploymentPath = [System.IO.Path]::GetFullPath([string]$plan.deployment.manifest.path)
[void](Assert-PBFileIdentity -Path $deploymentPath -Expected $plan.deployment.manifest -Name 'deployment manifest')
$deploymentVerificationText = @(& (Join-Path $PSScriptRoot 'Test-PBRemoteVisualDeploymentManifest.ps1') `
    -DeploymentManifestPath $deploymentPath 2>&1)
$deploymentVerification = ConvertFrom-PBStrictJsonText -Text ($deploymentVerificationText -join "`n") -Name 'deployment verification output'
if ($deploymentVerification.verified -isnot [bool] -or -not [bool]$deploymentVerification.verified -or
    [string]$deploymentVerification.runId -cne [string]$plan.runId)
{
    throw 'Frozen deployment manifest no longer verifies'
}
$uiEvidencePath = [System.IO.Path]::GetFullPath([string]$plan.remoteUi.evidence.path)
$uiEvidence = Read-PBBoundedJson -Path $uiEvidencePath -MaximumBytes 128KB
[void](Assert-PBFileIdentity -Path $uiEvidencePath -Expected $plan.remoteUi.evidence -Name 'remote UI evidence')
$uiVerificationText = @(& (Join-Path $PSScriptRoot 'Test-PBRemoteVisualUiEvidence.ps1') `
    -UiEvidencePath $uiEvidencePath -MetadataPath ([string]$plan.deployment.remoteMetadata.path) `
    -ScreenshotPath ([string]$plan.remoteUi.screenshot.path) -CaptureRecordPath ([string]$plan.remoteUi.captureRecord.path) `
    -PythonPath $resolvedPython 2>&1)
$uiVerification = ConvertFrom-PBStrictJsonText -Text ($uiVerificationText -join "`n") -Name 'remote UI verification output'
if ([string]$uiVerification.status -cne 'PASS' -or [string]$uiVerification.runId -cne [string]$plan.runId)
{
    throw 'Frozen remote UI visible evidence no longer verifies'
}

$encoderEndpoint = Read-EndpointProcess -Root $resolvedEncoderRoot -FileName 'encoder-process-result.json' -ExpectedRole Encoder -Plan $frozenPlan
$liveEndpoint = Read-EndpointProcess -Root $resolvedLiveRoot -FileName 'live-decoder-process-result.json' -ExpectedRole DecoderLive -Plan $frozenPlan
$offlineEndpoint = Read-EndpointProcess -Root $resolvedOfflineRoot -FileName 'offline-decoder-process-result.json' -ExpectedRole DecoderOffline -Plan $frozenPlan
if ([string]$plan.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2' -and
    ($null -eq $encoderEndpoint.monitorPreflightPath -or $null -eq $liveEndpoint.monitorPreflightPath -or
     $null -ne $offlineEndpoint.monitorPreflightPath))
{
    throw 'Step 21 live endpoint monitor-preflight artifact separation is invalid'
}
if ($null -ne $encoderEndpoint.publishedPath -or $null -ne $encoderEndpoint.replayPath -or
    $null -eq $liveEndpoint.publishedPath -or $null -eq $liveEndpoint.replayPath -or
    $null -eq $offlineEndpoint.publishedPath -or $null -ne $offlineEndpoint.replayPath)
{
    throw 'Endpoint artifact role separation is invalid'
}
Assert-EndpointArtifactInventory -Endpoint $encoderEndpoint -ExpectedRole Encoder
Assert-EndpointArtifactInventory -Endpoint $liveEndpoint -ExpectedRole DecoderLive
Assert-EndpointArtifactInventory -Endpoint $offlineEndpoint -ExpectedRole DecoderOffline
if ($null -eq $offlineEndpoint.process.replayInput -or
    $offlineEndpoint.process.replayInput -isnot [System.Collections.IDictionary] -or
    [UInt64]$offlineEndpoint.process.replayInput.size -ne [UInt64]$liveEndpoint.process.artifacts.replay.size -or
    [string]$offlineEndpoint.process.replayInput.sha256 -cne [string]$liveEndpoint.process.artifacts.replay.sha256 -or
    [System.IO.Path]::GetFullPath([string]$offlineEndpoint.process.replayInput.path) -ine [System.IO.Path]::GetFullPath($liveEndpoint.replayPath))
{
    throw 'Offline Decoder did not consume the exact live Replay v2 identity'
}

$encoderReport = $encoderEndpoint.report
$liveReport = $liveEndpoint.report
$offlineReport = $offlineEndpoint.report
$encoderMonitorSafetyOk = if ([string]$plan.profileToken -ceq 'remote-lf4')
{
    $encoderReport.monitorSafety.preflightPassed -is [bool] -and [bool]$encoderReport.monitorSafety.preflightPassed -and
        [string]$encoderReport.monitorSafety.status -ceq 'PASS'
}
else
{
    $encoderReport.monitorSafety.preflightPassed -is [bool] -and -not [bool]$encoderReport.monitorSafety.preflightPassed -and
        [string]$encoderReport.monitorSafety.status -ceq 'NotRequired'
}
if ([string]$encoderReport.schema -cne 'PixelBridge.RunReport.2' -or [string]$encoderReport.role -cne 'Encoder' -or
    [string]$encoderReport.runId -cne [string]$plan.runId -or [string]$encoderReport.state -cne 'Stopped' -or
    [string]$encoderReport.profile -cne [string]$plan.profileName -or [UInt64]$encoderReport.fileBytes -ne [UInt64]$plan.source.size -or
    [UInt32]$encoderReport.configuredLogicalVisualFps -ne [UInt32]$plan.logicalFps -or
    $encoderReport.sourceStable -isnot [bool] -or -not [bool]$encoderReport.sourceStable -or
    -not $encoderMonitorSafetyOk -or [string]$encoderReport.errorDetail -cne '')
{
    throw 'Encoder authoritative report contract failed'
}
Assert-DecoderReport -Report $liveReport -Plan $plan -Offline $false
Assert-DecoderReport -Report $offlineReport -Plan $plan -Offline $true
$expectedCommit = [string]$packageManifest.buildIdentity.headCommit
if ([string]$encoderReport.applicationName -cne 'PixelBridgeEncoder' -or
    [string]$liveReport.applicationName -cne 'PixelBridgeDecoder' -or
    [string]$offlineReport.applicationName -cne 'PixelBridgeDecoder' -or
    [string]$encoderReport.gitCommit -cne $expectedCommit -or [string]$liveReport.gitCommit -cne $expectedCommit -or
    [string]$offlineReport.gitCommit -cne $expectedCommit)
{
    throw 'Endpoint report application/build commit identity differs from the frozen package'
}
Assert-EndpointTimeline -Endpoint $encoderEndpoint -Report $encoderReport -Name 'Encoder endpoint'
Assert-EndpointTimeline -Endpoint $liveEndpoint -Report $liveReport -Name 'Live Decoder endpoint'
Assert-EndpointTimeline -Endpoint $offlineEndpoint -Report $offlineReport -Name 'Offline Decoder endpoint'
if ([System.IO.Path]::GetFullPath([string]$encoderReport.sourcePath) -ine $resolvedSource -or
    [System.IO.Path]::GetFullPath([string]$liveReport.outputPath) -ine [System.IO.Path]::GetFullPath($liveEndpoint.publishedPath) -or
    [System.IO.Path]::GetFullPath([string]$offlineReport.outputPath) -ine [System.IO.Path]::GetFullPath($offlineEndpoint.publishedPath) -or
    [System.IO.Path]::GetFullPath([string]$liveReport.replay.path) -ine [System.IO.Path]::GetFullPath($liveEndpoint.replayPath) -or
    [System.IO.Path]::GetFullPath([string]$offlineReport.replay.path) -ine [System.IO.Path]::GetFullPath([string]$offlineEndpoint.process.replayInput.path))
{
    throw 'Endpoint reports do not point to the exact persistent source/publish/Replay artifacts'
}
foreach ($field in @('sessionId', 'sessionTag', 'wholeFileDigest', 'originalFileBytes'))
{
    $encoderValue = if ($field -ceq 'originalFileBytes') { $encoderReport.fileBytes } else { $encoderReport[$field] }
    if ([string]$encoderValue -cne [string]$liveReport[$field] -or [string]$encoderValue -cne [string]$offlineReport[$field])
    {
        throw "Encoder/live/offline decisive identity mismatch: $field"
    }
}
if ([UInt64]$liveReport.replay.writtenFrames -ne [UInt64]$offlineReport.replay.offlineCaptureFrames -or
    [UInt64]$offlineReport.replay.offlineDemodResults -ne [UInt64]$liveReport.replay.writtenFrames -or
    [UInt64]$liveReport.replay.fileBytes -ne [UInt64]$liveEndpoint.process.artifacts.replay.size -or
    [UInt64]$offlineReport.replay.fileBytes -ne [UInt64]$liveEndpoint.process.artifacts.replay.size)
{
    throw 'Live Replay written-frame/demod/file identity differs from offline consumption'
}

$livePublishedIdentity = Get-PBFileIdentity -Path $liveEndpoint.publishedPath
$offlinePublishedIdentity = Get-PBFileIdentity -Path $offlineEndpoint.publishedPath
if ([UInt64]$livePublishedIdentity.size -ne [UInt64]$sourceIdentity.size -or
    [string]$livePublishedIdentity.sha256 -cne [string]$sourceIdentity.sha256 -or
    [UInt64]$offlinePublishedIdentity.size -ne [UInt64]$sourceIdentity.size -or
    [string]$offlinePublishedIdentity.sha256 -cne [string]$sourceIdentity.sha256)
{
    throw 'External source/live/offline length or SHA-256 differs'
}

$encoderJournal = Read-BoundedJournal -Path $encoderEndpoint.journalPath -ExpectedRole Encoder -RunId ([string]$plan.runId)
$liveJournal = Read-BoundedJournal -Path $liveEndpoint.journalPath -ExpectedRole Decoder -RunId ([string]$plan.runId)
$offlineJournal = Read-BoundedJournal -Path $offlineEndpoint.journalPath -ExpectedRole Decoder -RunId ([string]$plan.runId)
Assert-JournalTimeline -Endpoint $encoderEndpoint -Entries $encoderJournal -Name 'Encoder'
Assert-JournalTimeline -Endpoint $liveEndpoint -Entries $liveJournal -Name 'Live Decoder'
Assert-JournalTimeline -Endpoint $offlineEndpoint -Entries $offlineJournal -Name 'Offline Decoder'
if ([string]$encoderJournal[$encoderJournal.Count - 1].state -cne 'Stopped' -or
    [string]$liveJournal[$liveJournal.Count - 1].state -cne 'Completed' -or
    [string]$offlineJournal[$offlineJournal.Count - 1].state -cne 'Completed')
{
    throw 'Endpoint journal terminal states do not match the authoritative reports'
}
$decoderOffset = [Int64]$plan.policy.clock.decoderOffsetMilliseconds
$clockUncertainty = [Int64]$plan.policy.clock.uncertaintyMilliseconds
$adjustedLiveStart = [Int64]$liveReport.runStartedUnixMilliseconds + $decoderOffset
$adjustedLiveEnd = [Int64]$liveReport.runEndedUnixMilliseconds + $decoderOffset
$encoderStart = [Int64]$encoderReport.runStartedUnixMilliseconds
$encoderEnd = [Int64]$encoderReport.runEndedUnixMilliseconds
$warmupDelta = $encoderStart - $adjustedLiveStart
if ($warmupDelta -lt -$clockUncertainty -or
    $warmupDelta -gt [Int64]$plan.policy.decoderWarmupMaximumSeconds * 1000 + $clockUncertainty)
{
    throw 'Decoder-before-Encoder warmup interval exceeds the frozen plan and clock uncertainty'
}
if ($encoderEnd + $clockUncertainty -lt $adjustedLiveEnd)
{
    throw 'Encoder report ended before Decoder completed within the declared clock uncertainty'
}
$proofWindow = Get-PBPostDecoderBroadcastProofWindow `
    -DecoderEvidenceReadyUnixMilliseconds ([Int64]$liveEndpoint.process.evidenceReadyUnixMilliseconds) `
    -DecoderClockOffsetMilliseconds ([Int32]$decoderOffset) `
    -ProofSeconds ([UInt32]$plan.policy.postDecoderBroadcastProofSeconds)
$adjustedDecoderEvidenceReady = [Int64]$proofWindow.adjustedDecoderEvidenceReadyUnixMilliseconds
$requiredProofMilliseconds = [Int64]$proofWindow.requiredProofMilliseconds
$proofThreshold = [Int64]$proofWindow.thresholdUnixMilliseconds
$broadcastProof = @($encoderJournal | Where-Object {
    [string]$_.state -ceq 'Broadcasting' -and [Int64]$_.unixMs -ge $proofThreshold
})
if ($broadcastProof.Count -eq 0)
{
    throw 'Encoder journal lacks a Broadcasting sample after Decoder evidence-ready time plus the frozen proof interval'
}
$proofDuration = [Int64]$broadcastProof[$broadcastProof.Count - 1].unixMs - $adjustedDecoderEvidenceReady
if ($proofDuration -lt $requiredProofMilliseconds)
{
    throw 'Encoder post-Decoder Broadcasting proof duration is shorter than the frozen plan'
}

# Combined reports and the final seal contain artifact paths. Use the create-only final directory from the
# beginning so every persisted identity stays valid. The final seal is the completion marker; partial
# evidence remains inspectable and cannot be silently reused if verification stops early.
New-Item -ItemType Directory -Path $resolvedOutput | Out-Null
$combinedDirectory = Join-Path $resolvedOutput 'combined'
$reportTool = Join-Path (Split-Path -Parent $PSScriptRoot) 'PBRemoteVisualReport\pb_remote_visual_report.py'
if (-not (Test-Path -LiteralPath $reportTool -PathType Leaf))
{
    throw "Strict RemoteVisual report merger is missing: $reportTool"
}
$artifactPaths = @(
    $frozenPlan.path,
    $deploymentPath,
    $uiEvidencePath,
    [string]$plan.remoteUi.screenshot.path,
    [string]$plan.remoteUi.captureRecord.path,
    [string]$plan.deployment.remoteMetadata.path,
    [string]$plan.deployment.encoderEnvironment.path,
    [string]$plan.deployment.decoderEnvironment.path,
    [string]$plan.deployment.packageManifest.path,
    [string]$plan.deployment.sourceManifest.path,
    $encoderEndpoint.processPath,
    $liveEndpoint.processPath,
    $offlineEndpoint.processPath,
    $offlineEndpoint.reportPath
)
if ([string]$plan.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2')
{
    $artifactPaths += @($encoderEndpoint.monitorPreflightPath, $liveEndpoint.monitorPreflightPath)
}
$reportArguments = @(
    '-B', $reportTool,
    '--encoder', $encoderEndpoint.reportPath,
    '--decoder', $liveEndpoint.reportPath,
    '--output-dir', $combinedDirectory,
    '--source-file', $resolvedSource,
    '--published-file', $liveEndpoint.publishedPath,
    '--replay', $liveEndpoint.replayPath,
    '--decoder-clock-offset-ms', [string]$plan.policy.clock.decoderOffsetMilliseconds,
    '--clock-uncertainty-ms', [string]$plan.policy.clock.uncertaintyMilliseconds
)
foreach ($artifactPath in $artifactPaths)
{
    $reportArguments += @('--artifact', [System.IO.Path]::GetFullPath($artifactPath))
}
$reportToolStdout = Join-Path $resolvedOutput 'report-merger.stdout.log'
$reportToolStderr = Join-Path $resolvedOutput 'report-merger.stderr.log'
& $resolvedPython @reportArguments 1> $reportToolStdout 2> $reportToolStderr
$reportExitCode = $LASTEXITCODE
if ($reportExitCode -ne 0)
{
    throw "Strict RemoteVisual report merger failed with exit code $reportExitCode"
}
$combinedPath = Join-Path $combinedDirectory "remote-run-$($plan.runId)-combined.json"
$combined = Read-PBBoundedJson -Path $combinedPath -MaximumBytes 8MB
if ($combined.successfulRun -isnot [bool] -or -not [bool]$combined.successfulRun -or
    $combined.externalVerification.match -isnot [bool] -or -not [bool]$combined.externalVerification.match -or
    $combined.evidenceValid -isnot [bool] -or -not [bool]$combined.evidenceValid -or
    [string]$combined.runId -cne [string]$plan.runId)
{
    throw 'Strict merged report did not accept the authoritative live run'
}

$verification = [ordered]@{
    schema = 'PixelBridge.RemoteVisualPilotEvidenceVerification.1'
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    status = 'PASS'
    runId = [string]$plan.runId
    logicalFps = [UInt32]$plan.logicalFps
    geometryMode = [string]$plan.geometryMode
    completion = [ordered]@{
        wholeFileDigest = 'PASS'
        safePublish = 'PASS'
        externalLengthAndSha256 = 'PASS'
        replayOfflineReproduction = 'PASS'
        falseOutputCount = 0
        failureClassification = 'success'
    }
    decoderBeforeEncoderWarmupMilliseconds = $warmupDelta
    encoderBroadcastProof = [ordered]@{
        adjustedDecoderEvidenceReadyUnixMilliseconds = $adjustedDecoderEvidenceReady
        requiredAfterAdjustedDecoderEvidenceReadyUnixMilliseconds = $proofThreshold
        requiredProofMilliseconds = $requiredProofMilliseconds
        broadcastingSampleCount = $broadcastProof.Count
        lastBroadcastingSampleUnixMilliseconds = [Int64]$broadcastProof[$broadcastProof.Count - 1].unixMs
        provenDurationAfterAdjustedDecoderEvidenceReadyMilliseconds = $proofDuration
    }
    externalIdentity = [ordered]@{ source = $sourceIdentity; livePublished = $livePublishedIdentity; offlinePublished = $offlinePublishedIdentity }
    replayIdentity = Get-PBFileIdentity -Path $liveEndpoint.replayPath
    plan = $frozenPlan.identity
    deployment = Get-PBFileIdentity -Path $deploymentPath
    remoteUiEvidence = Get-PBFileIdentity -Path $uiEvidencePath
    endpointProcessResults = [ordered]@{
        encoder = Get-PBFileIdentity -Path $encoderEndpoint.processPath
        liveDecoder = Get-PBFileIdentity -Path $liveEndpoint.processPath
        offlineDecoder = Get-PBFileIdentity -Path $offlineEndpoint.processPath
    }
    endpointReports = [ordered]@{
        encoder = Get-PBFileIdentity -Path $encoderEndpoint.reportPath
        liveDecoder = Get-PBFileIdentity -Path $liveEndpoint.reportPath
        offlineDecoder = Get-PBFileIdentity -Path $offlineEndpoint.reportPath
    }
    endpointJournals = [ordered]@{
        encoder = Get-PBFileIdentity -Path $encoderEndpoint.journalPath
        liveDecoder = Get-PBFileIdentity -Path $liveEndpoint.journalPath
        offlineDecoder = Get-PBFileIdentity -Path $offlineEndpoint.journalPath
    }
    combinedReport = Get-PBFileIdentity -Path $combinedPath -RelativeTo $resolvedOutput
    reportMerger = Get-PBFileIdentity -Path $reportTool
}
if ([string]$plan.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2')
{
    $verification['profileToken'] = [string]$plan.profileToken
    $verification['captureBackend'] = [string]$plan.policy.captureBackend
    $verification['matrix'] = $plan.matrix
    $verification['monitorPreflights'] = [ordered]@{
        encoder = Get-PBFileIdentity -Path $encoderEndpoint.monitorPreflightPath
        liveDecoder = Get-PBFileIdentity -Path $liveEndpoint.monitorPreflightPath
    }
}
$verificationPath = Join-Path $resolvedOutput 'pilot-evidence-verification.json'
[void](Write-PBCreateOnlyJson -Path $verificationPath -Value $verification -Depth 30)
$matrixRecordPath = $null
if ([string]$plan.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2')
{
    $matrixRecord = New-PBRemoteVisualMatrixRunRecordValue -FrozenPlan $frozenPlan -Outcome Success `
        -FailureClassification success -SourcePath $resolvedSource -ReplayPath $liveEndpoint.replayPath `
        -CombinedReportPath $combinedPath -EncoderReportPath $encoderEndpoint.reportPath `
        -LiveDecoderReportPath $liveEndpoint.reportPath -OfflineDecoderReportPath $offlineEndpoint.reportPath `
        -OutcomeVerificationPath $verificationPath -InspectionPath $null
    $matrixRecordPath = Join-Path $resolvedOutput 'matrix-run-record.json'
    [void](Write-PBCreateOnlyJson -Path $matrixRecordPath -Value $matrixRecord -Depth 40)
    [void](Import-PBRemoteVisualMatrixRunRecord -Path $matrixRecordPath)
}
$sealInputs = [Collections.Generic.List[object]]::new()
foreach ($path in @(
    $frozenPlan.path, $deploymentPath, $uiEvidencePath, [string]$plan.remoteUi.screenshot.path,
    [string]$plan.remoteUi.captureRecord.path, [string]$plan.deployment.remoteMetadata.path,
    [string]$plan.deployment.encoderEnvironment.path, [string]$plan.deployment.decoderEnvironment.path,
    [string]$plan.deployment.packageManifest.path, [string]$plan.deployment.sourceManifest.path,
    $resolvedSource, $encoderEndpoint.processPath, $encoderEndpoint.reportPath, $encoderEndpoint.journalPath,
    $encoderEndpoint.stdoutPath, $encoderEndpoint.stderrPath,
    $liveEndpoint.processPath, $liveEndpoint.reportPath, $liveEndpoint.journalPath, $liveEndpoint.stdoutPath,
    $liveEndpoint.stderrPath, $liveEndpoint.publishedPath,
    $liveEndpoint.replayPath, $offlineEndpoint.processPath, $offlineEndpoint.reportPath, $offlineEndpoint.journalPath,
    $offlineEndpoint.stdoutPath, $offlineEndpoint.stderrPath, $offlineEndpoint.publishedPath, $combinedPath,
    (Join-Path $combinedDirectory "remote-run-$($plan.runId).md"),
    (Join-Path $combinedDirectory "remote-run-$($plan.runId).csv"),
    $verificationPath, $reportTool, $reportToolStdout, $reportToolStderr
))
{
    [void]$sealInputs.Add((Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($path))))
}
if ([string]$plan.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2')
{
    foreach ($path in @($encoderEndpoint.monitorPreflightPath, $liveEndpoint.monitorPreflightPath, $matrixRecordPath))
    {
        [void]$sealInputs.Add((Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($path))))
    }
}
$seal = [ordered]@{
    schema = 'PixelBridge.RemoteVisualPilotEvidenceSeal.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    runId = [string]$plan.runId
    status = 'PASS'
    artifactCount = $sealInputs.Count
    artifacts = @($sealInputs)
}
if ([string]$plan.schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2')
{
    $seal['profileToken'] = [string]$plan.profileToken
    $seal['captureBackend'] = [string]$plan.policy.captureBackend
    $seal['modeClass'] = [string]$plan.matrix.modeClass
    $seal['geometryMode'] = [string]$plan.geometryMode
    $seal['logicalFps'] = [UInt32]$plan.logicalFps
}
[void](Write-PBCreateOnlyJson -Path (Join-Path $resolvedOutput 'pilot-evidence-seal.json') -Value $seal -Depth 30)
[ordered]@{
    path = $resolvedOutput
    runId = [string]$plan.runId
    status = 'PASS'
    logicalFps = [UInt32]$plan.logicalFps
    geometryMode = [string]$plan.geometryMode
    profileToken = [string]$plan.profileToken
    captureBackend = [string]$plan.policy.captureBackend
    externalSha256 = [string]$sourceIdentity.sha256
    replaySha256 = [string](Get-FileHash -LiteralPath $liveEndpoint.replayPath -Algorithm SHA256).Hash.ToLowerInvariant()
    verificationSha256 = [string](Get-FileHash -LiteralPath (Join-Path $resolvedOutput 'pilot-evidence-verification.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    matrixRecordSha256 = if ($null -eq $matrixRecordPath) { $null } else { [string](Get-FileHash -LiteralPath $matrixRecordPath -Algorithm SHA256).Hash.ToLowerInvariant() }
    sealSha256 = [string](Get-FileHash -LiteralPath (Join-Path $resolvedOutput 'pilot-evidence-seal.json') -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json -Depth 10

#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,

    [Parameter(Mandatory = $true)]
    [string]$BaselineSummaryPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedBaselineSummarySha256,

    [Parameter(Mandatory = $true)]
    [string]$PackageManifestPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPackageManifestSha256,

    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-NewUtf8File
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try
    {
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
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

function Get-FileIdentity
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$RelativeTo
    )

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $item = Get-Item -LiteralPath $resolvedPath
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Evidence file is not a regular non-reparse file: $resolvedPath"
    }
    $reportedPath = if ([string]::IsNullOrEmpty($RelativeTo))
    {
        $resolvedPath
    }
    else
    {
        [IO.Path]::GetRelativePath($RelativeTo, $resolvedPath).Replace('\', '/')
    }
    return [ordered]@{
        path = $reportedPath
        size = [Int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Assert-Identity
{
    param(
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][object]$Actual,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$ComparePath
    )

    if ([Int64]$Expected.size -ne [Int64]$Actual.size -or [string]$Expected.sha256 -cne [string]$Actual.sha256 -or
        ($ComparePath -and [string]$Expected.path -cne [string]$Actual.path))
    {
        throw "$Name file identity mismatch"
    }
}

function Assert-NearlyEqual
{
    param(
        [Parameter(Mandatory = $true)][object]$Actual,
        [Parameter(Mandatory = $true)][double]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $actualNumber = [double]$Actual
    if ([double]::IsNaN($actualNumber) -or [double]::IsInfinity($actualNumber) -or
        [double]::IsNaN($Expected) -or [double]::IsInfinity($Expected))
    {
        throw "$Name is not finite"
    }
    $tolerance = [Math]::Max(1.0e-9, [Math]::Abs($Expected) * 1.0e-9)
    if ([Math]::Abs($actualNumber - $Expected) -gt $tolerance)
    {
        throw "$Name is inconsistent"
    }
}

function Assert-FiniteRange
{
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [double]$Minimum = 0.0,
        [double]$Maximum = [double]::PositiveInfinity,
        [switch]$ExclusiveMinimum
    )

    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or
        ($ExclusiveMinimum ? $number -le $Minimum : $number -lt $Minimum) -or $number -gt $Maximum)
    {
        throw "$Name is outside its numeric contract"
    }
    return $number
}

function Assert-GpuSummary
{
    param(
        [Parameter(Mandatory = $true)][object]$Summary,
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$MaximumSamples
    )

    if ([int]$Summary.sampleCount -lt 0 -or [int]$Summary.unavailableSampleCount -lt 0 -or
        [int]$Summary.sampleCount + [int]$Summary.unavailableSampleCount -gt $MaximumSamples)
    {
        throw "$Name GPU sample counts are invalid"
    }
    if ([bool]$Summary.available)
    {
        if ([int]$Summary.sampleCount -eq 0)
        {
            throw "$Name claims available GPU data without samples"
        }
        [void](Assert-FiniteRange -Value $Summary.averagePercent -Name "$Name GPU average")
        [void](Assert-FiniteRange -Value $Summary.peakPercent -Name "$Name GPU peak")
        if ([double]$Summary.averagePercent -gt [double]$Summary.peakPercent)
        {
            throw "$Name GPU average exceeds its peak"
        }
    }
    elseif ($null -ne $Summary.averagePercent -or $null -ne $Summary.peakPercent)
    {
        throw "$Name unavailable GPU summary contains numeric values"
    }
}

function Assert-CaseArtifacts
{
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $expectedArtifacts = @($Case.artifacts)
    $expectedPaths = @($expectedArtifacts | ForEach-Object { [string]$_.path })
    if ($expectedPaths.Count -ne (@($expectedPaths | Sort-Object -Unique)).Count)
    {
        throw "Duplicate case artifact path for $($Case.name)"
    }
    foreach ($artifact in $expectedArtifacts)
    {
        $artifactPath = Join-Path $Root ([string]$artifact.path)
        $actual = Get-FileIdentity -Path $artifactPath -RelativeTo $Root
        Assert-Identity -Expected $artifact -Actual $actual -Name "case artifact $($artifact.path)" -ComparePath
    }
}

function Get-VerifiedDecoderJournal
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RunId
    )

    $records = [Collections.Generic.List[object]]::new()
    $lastUnixMilliseconds = [Int64]::MinValue
    $lastResourceRejections = [UInt64]0
    foreach ($line in @(Get-Content -LiteralPath $Path))
    {
        if ([string]::IsNullOrWhiteSpace($line))
        {
            throw 'Decoder journal contains an empty record'
        }
        $record = $line | ConvertFrom-Json
        if ($record.schema -cne 'PixelBridge.RunJournal.1' -or $record.role -cne 'Decoder' -or
            $record.runId -cne $RunId -or [Int64]$record.unixMs -lt $lastUnixMilliseconds -or
            [UInt64]$record.outerResourceRejections -lt $lastResourceRejections -or
            [UInt64]$record.outerConflictRejections -ne 0)
        {
            throw 'Decoder journal identity, ordering, or conflict contract failed'
        }
        $lastUnixMilliseconds = [Int64]$record.unixMs
        $lastResourceRejections = [UInt64]$record.outerResourceRejections
        $records.Add($record)
    }
    if ($records.Count -lt 2)
    {
        throw 'Decoder journal does not contain periodic and terminal records'
    }
    $descriptorRecords = @($records | Where-Object { [bool]$_.descriptorKnown })
    $terminal = $records[$records.Count - 1]
    if ($descriptorRecords.Count -eq 0 -or $terminal.state -cne 'Completed' -or
        -not [bool]$terminal.wholeFileDigestPass -or -not [bool]$terminal.finalPublishPass)
    {
        throw 'Decoder journal did not reach authoritative completion'
    }
    $firstDescriptorRecord = $descriptorRecords[0]
    return [ordered]@{
        recordCount = $records.Count
        preDescriptorRecordCount = @($records | Where-Object { -not [bool]$_.descriptorKnown }).Count
        descriptorKnownRecordCount = $descriptorRecords.Count
        firstDescriptorKnownUnixMilliseconds = [Int64]$firstDescriptorRecord.unixMs
        resourceRejectionsAtFirstDescriptorSample = [UInt64]$firstDescriptorRecord.outerResourceRejections
        finalResourceRejections = [UInt64]$terminal.outerResourceRejections
        postDescriptorResourceRejectionIncrease = [UInt64]$terminal.outerResourceRejections -
            [UInt64]$firstDescriptorRecord.outerResourceRejections
        finalConflictRejections = [UInt64]$terminal.outerConflictRejections
        terminalState = [string]$terminal.state
        terminalWholeFileDigestPass = [bool]$terminal.wholeFileDigestPass
        terminalFinalPublishPass = [bool]$terminal.finalPublishPass
    }
}

$resolvedEvidenceRoot = (Resolve-Path -LiteralPath $EvidenceRoot).Path.TrimEnd('\')
$resolvedBaselineSummaryPath = (Resolve-Path -LiteralPath $BaselineSummaryPath).Path
$resolvedPackageManifestPath = (Resolve-Path -LiteralPath $PackageManifestPath).Path
if (-not [string]::IsNullOrEmpty($OutputPath))
{
    $resolvedOutputPath = [IO.Path]::GetFullPath($OutputPath)
    if ($resolvedOutputPath.StartsWith($resolvedEvidenceRoot + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Verification output must stay outside the sealed evidence root'
    }
}
else
{
    $resolvedOutputPath = $null
}

$sealPath = Join-Path $resolvedEvidenceRoot 'evidence-seal.json'
$sealIdentity = Get-FileIdentity -Path $sealPath
$seal = Get-Content -Raw -LiteralPath $sealPath | ConvertFrom-Json
if ($seal.schema -cne 'PixelBridge.LocalDesktopRegressionSeal.1' -or $seal.status -cne 'PASS' -or
    [int]$seal.artifactCount -ne @($seal.artifacts).Count)
{
    throw 'Evidence seal schema, status, or count is invalid'
}

$actualArtifacts = @(Get-ChildItem -LiteralPath $resolvedEvidenceRoot -Recurse -File -Force | Where-Object {
        $_.FullName -cne $sealPath
    } | Sort-Object { [IO.Path]::GetRelativePath($resolvedEvidenceRoot, $_.FullName).Replace('\', '/') } | ForEach-Object {
        Get-FileIdentity -Path $_.FullName -RelativeTo $resolvedEvidenceRoot
    })
$sealedArtifacts = @($seal.artifacts)
if ($actualArtifacts.Count -ne $sealedArtifacts.Count -or $actualArtifacts.Count -ne [int]$seal.artifactCount)
{
    throw 'Sealed evidence inventory count changed'
}
for ($index = 0; $index -lt $sealedArtifacts.Count; $index++)
{
    Assert-Identity -Expected $sealedArtifacts[$index] -Actual $actualArtifacts[$index] -Name "sealed artifact $index" -ComparePath
}

$baselineIdentity = Get-FileIdentity -Path $resolvedBaselineSummaryPath
$packageManifestIdentity = Get-FileIdentity -Path $resolvedPackageManifestPath
if ($baselineIdentity.sha256 -cne $ExpectedBaselineSummarySha256 -or
    $packageManifestIdentity.sha256 -cne $ExpectedPackageManifestSha256)
{
    throw 'External expected baseline or package hash is wrong'
}
Assert-Identity -Expected $seal.externalBindings.baselineSummary -Actual $baselineIdentity -Name 'sealed baseline'
Assert-Identity -Expected $seal.externalBindings.packageManifest -Actual $packageManifestIdentity -Name 'sealed package manifest'
$sourcePath = [string]$seal.externalBindings.source.path
$sourceIdentity = Get-FileIdentity -Path $sourcePath
Assert-Identity -Expected $seal.externalBindings.source -Actual $sourceIdentity -Name 'sealed source'
if ($sourceIdentity.size -ne 8MB -or $sourceIdentity.sha256 -cne 'e02206c8813b212705ce995f49498bfacdd49030f097492a6eb2777ea66a4fe2')
{
    throw 'Sealed source is not the frozen 8 MiB RAW corpus member'
}

$baseline = Get-Content -Raw -LiteralPath $resolvedBaselineSummaryPath | ConvertFrom-Json
$packageManifest = Get-Content -Raw -LiteralPath $resolvedPackageManifestPath | ConvertFrom-Json
$preflightPath = Join-Path $resolvedEvidenceRoot 'preflight.json'
$postSummaryPath = Join-Path $resolvedEvidenceRoot 'post-summary.json'
$comparisonCsvPath = Join-Path $resolvedEvidenceRoot 'comparison.csv'
$comparisonMarkdownPath = Join-Path $resolvedEvidenceRoot 'comparison.md'
$preflight = Get-Content -Raw -LiteralPath $preflightPath | ConvertFrom-Json
$post = Get-Content -Raw -LiteralPath $postSummaryPath | ConvertFrom-Json
if ($baseline.schema -cne 'PixelBridge.P1_5.PrechangeLocalDesktopReference.1' -or $baseline.status -cne 'PASS' -or
    $baseline.gitCommit -cne 'cd533feceaaf28b925e4b6878e589747ea159c67' -or
    $packageManifest.schema -cne 'PixelBridge.PortablePackage.2' -or
    $preflight.schema -cne 'PixelBridge.LocalDesktopRegressionPreflight.1' -or
    $post.schema -cne 'PixelBridge.LocalDesktopRegressionPost.1' -or $post.status -cne 'PASS' -or
    [int]$post.step -ne 19 -or -not [bool]$post.replayOff)
{
    throw 'Baseline, package, preflight, or post summary schema/status mismatch'
}

$actualPreflightIdentity = Get-FileIdentity -Path $preflightPath -RelativeTo $resolvedEvidenceRoot
Assert-Identity -Expected $post.preflight -Actual $actualPreflightIdentity -Name 'post preflight binding' -ComparePath
Assert-Identity -Expected $preflight.packageManifest -Actual $packageManifestIdentity -Name 'preflight package manifest'
Assert-Identity -Expected $preflight.baselineSummary -Actual $baselineIdentity -Name 'preflight baseline summary'
Assert-Identity -Expected $preflight.source -Actual $sourceIdentity -Name 'preflight source'
if ([string]$preflight.testedSourceFingerprintSha256 -cne [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256 -or
    [string]$preflight.buildIdentityFingerprintSha256 -cne [string]$packageManifest.buildIdentityFingerprintSha256 -or
    [string]$preflight.packagePayloadFingerprintSha256 -cne [string]$packageManifest.packagePayloadFingerprintSha256 -or
    [string]$post.testedSourceFingerprintSha256 -cne [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256 -or
    [string]$post.packageManifestSha256 -cne $packageManifestIdentity.sha256 -or
    [string]$post.baselineSummarySha256 -cne $baselineIdentity.sha256 -or [bool]$preflight.replay.enabled)
{
    throw 'Source/package fingerprint or replay preflight binding mismatch'
}
if ([string]$preflight.launchPolicy.order -cne 'DecoderBeforeEncoder' -or
    [UInt32]$preflight.launchPolicy.decoderWarmupMilliseconds -lt 100 -or
    [UInt32]$preflight.launchPolicy.decoderWarmupMilliseconds -gt 2000 -or
    [string]$post.launchPolicy.order -cne [string]$preflight.launchPolicy.order -or
    [UInt32]$post.launchPolicy.decoderWarmupMilliseconds -ne [UInt32]$preflight.launchPolicy.decoderWarmupMilliseconds -or
    [string]$post.launchPolicy.rationale -cne [string]$preflight.launchPolicy.rationale -or
    [string]$post.launchPolicy.verifiedEncodedGoodputWindow -cne [string]$preflight.launchPolicy.verifiedEncodedGoodputWindow)
{
    throw 'Decoder-before-Encoder launch-alignment contract is missing or inconsistent'
}

$encoderApplication = @($packageManifest.applications | Where-Object { $_.role -ceq 'Encoder' })
$decoderApplication = @($packageManifest.applications | Where-Object { $_.role -ceq 'Decoder' })
if ($encoderApplication.Count -ne 1 -or $decoderApplication.Count -ne 1)
{
    throw 'Package application roles are ambiguous'
}
$encoderExecutable = Get-FileIdentity -Path ([string]$preflight.encoder.path)
$decoderExecutable = Get-FileIdentity -Path ([string]$preflight.decoder.path)
Assert-Identity -Expected $preflight.encoder -Actual $encoderExecutable -Name 'Encoder executable'
Assert-Identity -Expected $preflight.decoder -Actual $decoderExecutable -Name 'Decoder executable'
if ($encoderExecutable.sha256 -cne [string]$encoderApplication[0].sha256 -or
    $decoderExecutable.sha256 -cne [string]$decoderApplication[0].sha256)
{
    throw 'Runtime executables do not match the package applications'
}

$expectedCaseNames = @('wgc-direct', 'dxgi-direct', 'wgc-shape', 'dxgi-shape')
$baselineCases = @($baseline.cases)
$postCases = @($post.cases)
$comparisonRows = @($post.comparison)
foreach ($collection in @($baselineCases, $postCases, $comparisonRows))
{
    $names = @($collection | ForEach-Object { [string]$_.name })
    if ($names.Count -ne 4 -or (@($names | Sort-Object) -join ',') -cne (@($expectedCaseNames | Sort-Object) -join ','))
    {
        throw 'A matrix collection does not contain exactly the four required cases'
    }
}

foreach ($caseName in $expectedCaseNames)
{
    $baselineCase = @($baselineCases | Where-Object { $_.name -ceq $caseName })[0]
    $postCase = @($postCases | Where-Object { $_.name -ceq $caseName })[0]
    $comparison = @($comparisonRows | Where-Object { $_.name -ceq $caseName })[0]
    $caseSummaryPath = Join-Path $resolvedEvidenceRoot (Join-Path $caseName 'case-summary.json')
    $caseSummary = Get-Content -Raw -LiteralPath $caseSummaryPath | ConvertFrom-Json
    if (($caseSummary | ConvertTo-Json -Depth 40 -Compress) -cne ($postCase | ConvertTo-Json -Depth 40 -Compress))
    {
        throw "Embedded and standalone case summaries differ for $caseName"
    }
    Assert-CaseArtifacts -Case $postCase -Root $resolvedEvidenceRoot

    $caseSourceIdentity = Get-FileIdentity -Path ([string]$postCase.source.path)
    Assert-Identity -Expected $postCase.source -Actual $caseSourceIdentity -Name "$caseName source"
    Assert-Identity -Expected $sourceIdentity -Actual $caseSourceIdentity -Name "$caseName frozen source"
    $outputPath = Join-Path $resolvedEvidenceRoot ([string]$postCase.output.path)
    $outputIdentity = Get-FileIdentity -Path $outputPath -RelativeTo $resolvedEvidenceRoot
    Assert-Identity -Expected $postCase.output -Actual $outputIdentity -Name "$caseName output" -ComparePath
    Assert-Identity -Expected $sourceIdentity -Actual $outputIdentity -Name "$caseName external hash"

    $encoderReportPath = Join-Path $resolvedEvidenceRoot ([string]$postCase.encoder.report.path)
    $decoderReportPath = Join-Path $resolvedEvidenceRoot ([string]$postCase.decoder.report.path)
    $decoderJournalPath = Join-Path $resolvedEvidenceRoot ([string]$postCase.decoder.journal.path)
    $encoderReportIdentity = Get-FileIdentity -Path $encoderReportPath -RelativeTo $resolvedEvidenceRoot
    $decoderReportIdentity = Get-FileIdentity -Path $decoderReportPath -RelativeTo $resolvedEvidenceRoot
    $decoderJournalIdentity = Get-FileIdentity -Path $decoderJournalPath -RelativeTo $resolvedEvidenceRoot
    Assert-Identity -Expected $postCase.encoder.report -Actual $encoderReportIdentity -Name "$caseName Encoder report" -ComparePath
    Assert-Identity -Expected $postCase.decoder.report -Actual $decoderReportIdentity -Name "$caseName Decoder report" -ComparePath
    Assert-Identity -Expected $postCase.decoder.journal -Actual $decoderJournalIdentity -Name "$caseName Decoder journal" -ComparePath
    $encoderReport = Get-Content -Raw -LiteralPath $encoderReportPath | ConvertFrom-Json
    $decoderReport = Get-Content -Raw -LiteralPath $decoderReportPath | ConvertFrom-Json
    $expectedActualBackend = if ($postCase.backend -ceq 'dxgi') { 'DXGI Desktop Duplication' } else { 'WGC' }
    if ($encoderReport.schema -cne 'PixelBridge.RunReport.2' -or $decoderReport.schema -cne 'PixelBridge.RunReport.2' -or
        $encoderReport.runId -cne [string]$postCase.runId -or $decoderReport.runId -cne [string]$postCase.runId -or
        $encoderReport.state -cne 'Stopped' -or $decoderReport.state -cne 'Completed' -or
        [string]$encoderReport.sessionId -cne [string]$decoderReport.sessionId -or
        [UInt64]$encoderReport.sessionTag -ne [UInt64]$decoderReport.sessionTag -or
        [UInt64]$encoderReport.visualProfileId -ne [UInt64]$decoderReport.visualProfileId -or
        [int]$encoderReport.visualLayoutVersion -ne [int]$decoderReport.visualLayoutVersion -or
        [string]$encoderReport.remoteMetadata.channelType -cne 'LocalDesktop' -or
        [string]$decoderReport.remoteMetadata.channelType -cne 'LocalDesktop' -or
        [string]$decoderReport.actualBackend -cne $expectedActualBackend -or
        -not [bool]$encoderReport.sourceStable -or -not [bool]$decoderReport.wholeFileDigestVerified -or
        -not [bool]$decoderReport.finalPublishSucceeded -or [bool]$decoderReport.replay.enabled -or
        [bool]$decoderReport.replay.offlineMode -or [UInt64]$decoderReport.outerAdmission.conflictRejections -ne 0 -or
        [UInt64]$decoderReport.crcFailures -ne 0 -or [UInt64]$decoderReport.identityFailures -ne 0 -or
        -not [bool]$decoderReport.evidence.journalEnabled -or -not [bool]$decoderReport.evidence.valid -or
        [bool]$decoderReport.evidence.journalTruncated -or -not [bool]$decoderReport.evidence.journalFinished -or
        [string]$decoderReport.wholeFileDigest -notmatch '^[0-9a-f]{64}$' -or
        [string]$decoderReport.wholeFileDigest -cne [string]$encoderReport.wholeFileDigest -or
        [Int64]$decoderReport.originalFileBytes -ne $sourceIdentity.size -or [Int64]$decoderReport.verifiedRawBytes -ne $sourceIdentity.size)
    {
        throw "Authoritative report contract failed for $caseName"
    }
    if ([bool]$postCase.replayEnabled -or [string]$postCase.decoderState -cne 'Completed' -or
        -not [bool]$postCase.wholeFileDigestVerified -or -not [bool]$postCase.finalPublishSucceeded -or
        -not [bool]$postCase.sourceOutputLengthEqual -or -not [bool]$postCase.sourceOutputSha256Equal -or
        -not [bool]$postCase.encoder.aliveWhenDecoderCompleted -or [bool]$postCase.screenSafety.focusViolation -or
        [bool]$postCase.screenSafety.containmentViolation -or [int]$postCase.encoder.exitCode -ne 0 -or
        [int]$postCase.decoder.exitCode -ne 0)
    {
        throw "Completion, broadcast, or screen-safety summary failed for $caseName"
    }
    $decoderStarted = [DateTimeOffset]$postCase.launch.decoderStartedUtc
    $encoderStarted = [DateTimeOffset]$postCase.launch.encoderStartedUtc
    $actualLeadMilliseconds = ($encoderStarted - $decoderStarted).TotalMilliseconds
    $journal = Get-VerifiedDecoderJournal -Path $decoderJournalPath -RunId ([string]$postCase.runId)
    if ([string]$postCase.launch.order -cne 'DecoderBeforeEncoder' -or
        [UInt32]$postCase.launch.configuredDecoderWarmupMilliseconds -ne [UInt32]$preflight.launchPolicy.decoderWarmupMilliseconds -or
        [double]$postCase.launch.observedDecoderLeadMilliseconds -lt [double]$preflight.launchPolicy.decoderWarmupMilliseconds * 0.8 -or
        [Math]::Abs([double]$postCase.launch.observedDecoderLeadMilliseconds - $actualLeadMilliseconds) -gt 5.0 -or
        [UInt64]$postCase.outerAdmission.uniqueSymbols -ne [UInt64]$decoderReport.outerAdmission.uniqueSymbols -or
        [UInt64]$postCase.outerAdmission.identicalDuplicateSymbols -ne [UInt64]$decoderReport.outerAdmission.identicalDuplicateSymbols -or
        [UInt64]$postCase.outerAdmission.recoveryAlreadyReadySymbols -ne [UInt64]$decoderReport.outerAdmission.recoveryAlreadyReadySymbols -or
        [UInt64]$postCase.outerAdmission.alreadyCompletedSymbols -ne [UInt64]$decoderReport.outerAdmission.alreadyCompletedSymbols -or
        [UInt64]$postCase.outerAdmission.resourceRejections -ne [UInt64]$decoderReport.outerAdmission.resourceRejections -or
        [UInt64]$postCase.outerAdmission.conflictRejections -ne 0 -or
        [UInt64]$decoderReport.evidence.journalSamples -ne [UInt64]$journal.recordCount -or
        [UInt64]$journal.finalResourceRejections -ne [UInt64]$decoderReport.outerAdmission.resourceRejections -or
        [UInt64]$journal.postDescriptorResourceRejectionIncrease -ne 0)
    {
        throw "Launch alignment or Outer admission summary failed for $caseName"
    }
    foreach ($property in @('recordCount', 'preDescriptorRecordCount', 'descriptorKnownRecordCount',
            'firstDescriptorKnownUnixMilliseconds', 'resourceRejectionsAtFirstDescriptorSample',
            'finalResourceRejections', 'postDescriptorResourceRejectionIncrease', 'finalConflictRejections'))
    {
        if ([UInt64]$postCase.journalAnalysis.$property -ne [UInt64]$journal.$property)
        {
            throw "Journal analysis property mismatch for $caseName`: $property"
        }
    }
    if ([string]$postCase.journalAnalysis.terminalState -cne [string]$journal.terminalState -or
        [bool]$postCase.journalAnalysis.terminalWholeFileDigestPass -ne [bool]$journal.terminalWholeFileDigestPass -or
        [bool]$postCase.journalAnalysis.terminalFinalPublishPass -ne [bool]$journal.terminalFinalPublishPass)
    {
        throw "Journal terminal analysis mismatch for $caseName"
    }

    $goodput = Assert-FiniteRange -Value $decoderReport.verifiedEncodedGoodputBitsPerSecond -Name "$caseName goodput" -ExclusiveMinimum
    $fer = Assert-FiniteRange -Value $decoderReport.fecFrameErrorRate -Name "$caseName FER" -Maximum 1.0
    $uniqueVisualFps = Assert-FiniteRange -Value $decoderReport.uniqueVisualFps -Name "$caseName UniqueVisualFPS" -ExclusiveMinimum
    $admittedFrameSequenceFps = Assert-FiniteRange -Value $decoderReport.admittedFrameSequenceFps -Name "$caseName admitted FrameSequence FPS" -ExclusiveMinimum
    Assert-NearlyEqual -Actual $postCase.verifiedEncodedGoodputBitsPerSecond -Expected $goodput -Name "$caseName case goodput"
    Assert-NearlyEqual -Actual $postCase.fecFrameErrorRate -Expected $fer -Name "$caseName case FER"
    Assert-NearlyEqual -Actual $postCase.uniqueVisualFps -Expected $uniqueVisualFps -Name "$caseName case UniqueVisualFPS"
    Assert-NearlyEqual -Actual $postCase.admittedFrameSequenceFps -Expected $admittedFrameSequenceFps -Name "$caseName case admitted FPS"
    if ([UInt64]$postCase.frameLeaseHighWater -gt 6 -or [UInt64]$postCase.demodPendingHighWater -gt 4 -or
        [UInt64]$postCase.resultQueueHighWater -gt 128)
    {
        throw "Bounded queue high-water failed for $caseName"
    }

    Assert-GpuSummary -Summary $postCase.encoder.gpu -Name "$caseName Encoder" -MaximumSamples ([int]$postCase.resourceSampleCount)
    Assert-GpuSummary -Summary $postCase.decoder.gpu -Name "$caseName Decoder" -MaximumSamples ([int]$postCase.resourceSampleCount)
    foreach ($role in @('encoder', 'decoder'))
    {
        [void](Assert-FiniteRange -Value $postCase.$role.cpuEquivalentCores -Name "$caseName $role CPU" -ExclusiveMinimum)
        foreach ($property in @('privateBytes', 'workingSetBytes', 'handles'))
        {
            [void](Assert-FiniteRange -Value $postCase.$role.resourceHighWater.$property -Name "$caseName $role $property" -ExclusiveMinimum)
        }
    }

    $baselineOutputIdentity = Get-FileIdentity -Path ([string]$baselineCase.output)
    Assert-Identity -Expected $sourceIdentity -Actual $baselineOutputIdentity -Name "$caseName baseline external output"
    $baselineGoodput = Assert-FiniteRange -Value $baselineCase.verifiedEncodedGoodputBitsPerSecond -Name "$caseName baseline goodput" -ExclusiveMinimum
    $expectedRatio = $goodput / $baselineGoodput
    Assert-NearlyEqual -Actual $comparison.baselineGoodputBitsPerSecond -Expected $baselineGoodput -Name "$caseName comparison baseline goodput"
    Assert-NearlyEqual -Actual $comparison.postGoodputBitsPerSecond -Expected $goodput -Name "$caseName comparison post goodput"
    Assert-NearlyEqual -Actual $comparison.goodputRatio -Expected $expectedRatio -Name "$caseName comparison ratio"
    Assert-NearlyEqual -Actual $comparison.goodputChangePercent -Expected (($expectedRatio - 1.0) * 100.0) -Name "$caseName comparison percent"
    if ($expectedRatio -lt 0.9 -or -not [bool]$comparison.goodputRegressionWithinTenPercent -or
        -not [bool]$comparison.externalHashPass -or -not [bool]$comparison.wholeFileDigestPass -or
        -not [bool]$comparison.finalPublishPass -or -not [bool]$comparison.encoderStillBroadcastingAtCompletion -or
        -not [bool]$comparison.replayOff -or -not [bool]$comparison.launchAlignmentPass -or
        [UInt64]$comparison.postOuterResourceRejections -ne [UInt64]$postCase.outerAdmission.resourceRejections -or
        [UInt64]$comparison.postDescriptorResourceRejectionIncrease -ne 0 -or
        -not [bool]$comparison.boundedHighWaterPass)
    {
        throw "Step 19 acceptance failed for $caseName"
    }
}

foreach ($property in @('fourCaseWholeFileDigestPublishExternalHash', 'encoderStillBroadcastingAllCases',
        'replayOffAllCases', 'decoderBeforeEncoderAndNoPostDescriptorResourceIncreaseAllCases',
        'boundedHighWaterAllCases', 'goodputRegressionWithinTenPercentAllCases'))
{
    if (-not [bool]$post.acceptance.$property)
    {
        throw "Post acceptance property is false: $property"
    }
}

$csvRows = @(Import-Csv -LiteralPath $comparisonCsvPath)
if ($csvRows.Count -ne 4 -or (@($csvRows.name | Sort-Object) -join ',') -cne (@($expectedCaseNames | Sort-Object) -join ','))
{
    throw 'Comparison CSV does not contain the exact matrix'
}
foreach ($csvRow in $csvRows)
{
    $comparison = @($comparisonRows | Where-Object { $_.name -ceq $csvRow.name })[0]
    Assert-NearlyEqual -Actual $csvRow.postGoodputBitsPerSecond -Expected ([double]$comparison.postGoodputBitsPerSecond) -Name "$($csvRow.name) CSV goodput"
    if ([bool]::Parse($csvRow.goodputRegressionWithinTenPercent) -ne [bool]$comparison.goodputRegressionWithinTenPercent -or
        [bool]::Parse($csvRow.externalHashPass) -ne [bool]$comparison.externalHashPass -or
        [bool]::Parse($csvRow.launchAlignmentPass) -ne [bool]$comparison.launchAlignmentPass -or
        [UInt64]$csvRow.postOuterResourceRejections -ne [UInt64]$comparison.postOuterResourceRejections -or
        [UInt64]$csvRow.postDescriptorResourceRejectionIncrease -ne [UInt64]$comparison.postDescriptorResourceRejectionIncrease)
    {
        throw "Comparison CSV boolean mismatch for $($csvRow.name)"
    }
}
$markdown = Get-Content -Raw -LiteralPath $comparisonMarkdownPath
if ($markdown -notmatch 'Status: \*\*PASS\*\*')
{
    throw 'Comparison Markdown does not report PASS'
}
foreach ($caseName in $expectedCaseNames)
{
    if ($markdown -notmatch [regex]::Escape($caseName))
    {
        throw "Comparison Markdown omits $caseName"
    }
}

$partialFiles = @(Get-ChildItem -LiteralPath $resolvedEvidenceRoot -Recurse -File -Force | Where-Object {
        $_.Name -like '*.part' -or $_.Name -like '*.partial' -or $_.Name -like '*.pbrv2'
    })
if ($partialFiles.Count -ne 0)
{
    throw 'Sealed Step 19 evidence contains a partial or Replay artifact'
}

$verification = [ordered]@{
    schema = 'PixelBridge.LocalDesktopRegressionVerification.1'
    verified = $true
    verifiedUtc = [DateTime]::UtcNow.ToString('O')
    evidenceRoot = $resolvedEvidenceRoot
    evidenceSeal = $sealIdentity
    baselineSummarySha256 = $baselineIdentity.sha256
    packageManifestSha256 = $packageManifestIdentity.sha256
    testedSourceFingerprintSha256 = [string]$post.testedSourceFingerprintSha256
    sourceSha256 = $sourceIdentity.sha256
    caseCount = 4
    status = 'PASS'
}
$json = ($verification | ConvertTo-Json -Depth 8) + [Environment]::NewLine
if ($null -ne $resolvedOutputPath)
{
    Write-NewUtf8File -Path $resolvedOutputPath -Content $json
}
$json

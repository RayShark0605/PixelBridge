#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackageSealPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPackageManifestSha256,

    [Parameter(Mandatory = $true)]
    [string]$SourceSetDirectory,

    [Parameter(Mandatory = $true)]
    [string]$SourceSetSealPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedSourceManifestSha256,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ExpectedGitCommit,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'PBRemoteVisualStep22Common.psm1') -Force -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1') -ErrorAction Stop

function Require-Condition
{
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition)
    {
        throw $Message
    }
}

function Test-IsWithin
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $resolvedPath.Equals($resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $resolvedPath.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)
}

function New-CsprngId
{
    $bytes = [byte[]]::new(16)
    $algorithm = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try
    {
        $algorithm.GetBytes($bytes)
        return ([BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally
    {
        $algorithm.Dispose()
        [Array]::Clear($bytes, 0, $bytes.Length)
    }
}

$resolvedPackage = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedSourceSet = [System.IO.Path]::GetFullPath($SourceSetDirectory).TrimEnd('\')
$resolvedSourceSetSeal = [System.IO.Path]::GetFullPath($SourceSetSealPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryOutput = "$resolvedOutput.partial"
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
{
    throw "Create-only Step 22 campaign or partial already exists: $resolvedOutput"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Step 22 campaign parent directory does not exist: $outputParent"
}
$outputParentItem = Get-Item -LiteralPath $outputParent
if ($outputParentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
{
    throw 'Step 22 campaign parent must not be a reparse point'
}
if ((Test-IsWithin -Path $resolvedOutput -Root $resolvedPackage) -or
    (Test-IsWithin -Path $resolvedOutput -Root $resolvedSourceSet))
{
    throw 'Step 22 campaign output must remain outside the immutable package and source set'
}

$packageVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
$packageVerificationText = @(& $packageVerifier -PackageDirectory $resolvedPackage `
    -PackageSealPath $resolvedPackageSeal -ExpectedManifestSha256 $ExpectedPackageManifestSha256) -join "`n"
$packageVerification = ConvertFrom-PBStrictJsonText -Text $packageVerificationText -Name 'Step 22 package verification output'
Require-Condition -Condition ($packageVerification.verified -is [bool] -and [bool]$packageVerification.verified) `
    -Message 'Step 22 package verifier did not return a verified result'

$sourceVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
$sourceVerificationText = @(& $sourceVerifier -SourceSetDirectory $resolvedSourceSet `
    -SourceSealPath $resolvedSourceSetSeal -ExpectedManifestSha256 $ExpectedSourceManifestSha256) -join "`n"
$sourceVerification = ConvertFrom-PBStrictJsonText -Text $sourceVerificationText -Name 'Step 22 source-set verification output'
Require-Condition -Condition ($sourceVerification.verified -is [bool] -and [bool]$sourceVerification.verified) `
    -Message 'Step 22 source-set verifier did not return a verified result'

$packageManifestPath = Join-Path $resolvedPackage 'package-manifest.json'
$sourceManifestPath = Join-Path $resolvedSourceSet 'source-manifest.json'
$packageManifestIdentity = Get-PBFileIdentity -Path $packageManifestPath
$packageSealIdentity = Get-PBFileIdentity -Path $resolvedPackageSeal
$sourceManifestIdentity = Get-PBFileIdentity -Path $sourceManifestPath
$sourceSealIdentity = Get-PBFileIdentity -Path $resolvedSourceSetSeal
$packageManifest = Read-PBBoundedJson -Path $packageManifestPath -MaximumBytes 8MB
$sourceManifest = Read-PBBoundedJson -Path $sourceManifestPath -MaximumBytes 2MB
Require-Condition -Condition ([string]$packageManifestIdentity.sha256 -ceq $ExpectedPackageManifestSha256 -and
    [string]$sourceManifestIdentity.sha256 -ceq $ExpectedSourceManifestSha256 -and
    [string]$packageManifest.buildIdentity.headCommit -ceq $ExpectedGitCommit) `
    -Message 'Step 22 package/source expected identity mismatch'

$applications = @()
foreach ($role in @('Encoder', 'Decoder'))
{
    $matches = @($packageManifest.applications | Where-Object { [string]$_.role -ceq $role })
    if ($matches.Count -ne 1)
    {
        throw "Step 22 package must contain exactly one $role application"
    }
    $application = $matches[0]
    $applications += [ordered]@{
        role = [string]$application.role
        application = [string]$application.application
        relativeExecutablePath = [string]$application.relativeExecutablePath
        size = [UInt64]$application.size
        sha256 = [string]$application.sha256
        versionOutput = [string]$application.versionOutput
    }
}

$sourceFiles = @()
$sourceMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
foreach ($relativePath in @('random-1MiB.bin', 'random-8MiB.bin', 'random-payload-4MiB.zip'))
{
    $matches = @($sourceManifest.files | Where-Object { [string]$_.path -ceq $relativePath })
    if ($matches.Count -ne 1)
    {
        throw "Step 22 sealed source is missing or duplicated: $relativePath"
    }
    $source = [ordered]@{
        relativePath = $relativePath
        size = [UInt64]$matches[0].size
        sha256 = [string]$matches[0].sha256
        pixelBridgeSegmentCompression = [string]$matches[0].pixelBridgeSegmentCompression
    }
    $sourceFiles += $source
    [void]$sourceMap.Add($relativePath, $source)
}

$allocatedIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$campaignId = New-CsprngId
[void]$allocatedIds.Add($campaignId)
$runs = @()
foreach ($definition in @(Get-PBStep22RequiredRunDefinitions))
{
    do
    {
        $runId = New-CsprngId
    }
    while (-not $allocatedIds.Add($runId))
    $source = $sourceMap[[string]$definition.sourceRelativePath]
    $runs += [ordered]@{
        ordinal = [UInt32]$definition.ordinal
        slotId = [string]$definition.slotId
        runId = $runId
        status = 'PENDING'
        sourceClass = [string]$definition.sourceClass
        sourceRelativePath = [string]$definition.sourceRelativePath
        sourceSize = [UInt64]$source.size
        sourceSha256 = [string]$source.sha256
        successOrdinalWithinSource = [UInt32]$definition.successOrdinalWithinSource
        evidenceDirectoryName = $runId
    }
}

$campaign = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep22Campaign.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'READY_NOT_EXECUTED'
    campaignId = $campaignId
    runtime = [ordered]@{
        gitCommit = [string]$packageManifest.buildIdentity.headCommit
        headTree = [string]$packageManifest.buildIdentity.headTree
        packageManifestSha256 = [string]$packageManifestIdentity.sha256
        packageSealSha256 = [string]$packageSealIdentity.sha256
        buildIdentityFingerprintSha256 = [string]$packageManifest.buildIdentityFingerprintSha256
        packagePayloadFingerprintSha256 = [string]$packageManifest.packagePayloadFingerprintSha256
        applications = $applications
    }
    sourceSet = [ordered]@{
        sourceSetId = [string]$sourceManifest.sourceSetId
        sourceSetFingerprintSha256 = [string]$sourceManifest.sourceSetFingerprintSha256
        manifestSha256 = [string]$sourceManifestIdentity.sha256
        sealSha256 = [string]$sourceSealIdentity.sha256
        files = $sourceFiles
        zipSourcePayload = [ordered]@{
            archiveRelativePath = [string]$sourceManifest.zipSourcePayload.archivePath
            entryPath = [string]$sourceManifest.zipSourcePayload.entryPath
            size = [UInt64]$sourceManifest.zipSourcePayload.size
            sha256 = [string]$sourceManifest.zipSourcePayload.sha256
        }
    }
    profile = [ordered]@{
        token = 'remote-lf4'
        name = 'PB-RemoteVisual-LF4-X1 (Experimental)'
        visualProfileId = [UInt64]5783275402097472561
        visualLayoutVersion = [UInt32]7
        codedDataBytesPerFrame = [UInt32]8100
        codewordsPerFrame = [UInt32]4
        outerFec = 'Wirehair V2'
        innerFec = 'Robust DVB-S2 Short QC-LDPC'
    }
    policy = [ordered]@{
        configurationName = 'Step21ProvenReliableLF4TwoHertz'
        channelType = 'RemoteVisual'
        remoteProvider = 'UserProvidedVisualLink'
        compression = 'off'
        segmentCompression = 'RAW/OFF'
        captureBackend = 'wgc'
        encoderMonitorSelector = 'primary'
        decoderRoiSelection = 'WholeExperimentMonitorAtRunStart'
        logicalVisualFps = [UInt32]2
        controlRepetitions = [UInt32]12
        decoderTimeoutSeconds = [UInt32]3600
        noProgressSeconds = [UInt32]180
        replayPolicy = 'DisabledForBoundedLiveFileSmoke'
        decoderStartsBeforeEncoder = $true
        manualSenderStopAfterReceiverCompletion = $true
        noNonVisualPayloadPath = $true
    }
    runs = $runs
    acceptance = [ordered]@{
        requiredRunCount = [UInt32]6
        random1MiBSuccessesRequired = [UInt32]3
        random8MiBSuccessesRequired = [UInt32]2
        zip4MiBPayloadSuccessesRequired = [UInt32]1
        allScheduledRunsMustPass = $true
        preserveFailedEvidence = $true
        reuseRunIdForbidden = $true
        senderCyclePositionIsCompletion = $false
        remoteVisualSmokePassRequiresFinalVerifier = $true
    }
    truthBoundary = [ordered]@{
        executedRunCount = [UInt32]0
        successfulRunCount = [UInt32]0
        remoteVisualSmokePass = $false
        certifiedRemoteVisualProfile = $false
        crossBrandCoverageClaimed = $false
        crossModeCoverageClaimed = $false
        statement = 'This create-only campaign preallocates six required run identities but executes no Encoder, Decoder, capture, Receiver, digest, or publish operation.'
    }
}

try
{
    $temporaryIdentity = Write-PBCreateOnlyJson -Path $temporaryOutput -Value $campaign -Depth 32
    $campaignVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualStep22Campaign.ps1'
    $verificationText = @(& $campaignVerifier -CampaignPath $temporaryOutput `
        -ExpectedCampaignSha256 ([string]$temporaryIdentity.sha256) -PackageDirectory $resolvedPackage `
        -PackageSealPath $resolvedPackageSeal -SourceSetDirectory $resolvedSourceSet `
        -SourceSetSealPath $resolvedSourceSetSeal) -join "`n"
    $verification = ConvertFrom-PBStrictJsonText -Text $verificationText -Name 'Step 22 campaign self-verification output'
    Require-Condition -Condition ($verification.verified -is [bool] -and [bool]$verification.verified -and
        [string]$verification.campaignId -ceq $campaignId -and [UInt32]$verification.runCount -eq 6) `
        -Message 'Step 22 campaign self-verifier did not accept the exact temporary artifact'
    Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
}
finally
{
    if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
    {
        [System.IO.File]::Delete($temporaryOutput)
    }
}

$outputIdentity = Get-PBFileIdentity -Path $resolvedOutput
[ordered]@{
    path = $resolvedOutput
    size = [UInt64]$outputIdentity.size
    sha256 = [string]$outputIdentity.sha256
    schema = 'PixelBridge.RemoteVisualStep22Campaign.1'
    status = 'READY_NOT_EXECUTED'
    campaignId = $campaignId
    runCount = [UInt32]$runs.Count
    remoteVisualSmokePass = $false
    certifiedRemoteVisualProfile = $false
} | ConvertTo-Json -Depth 8

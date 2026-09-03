#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CampaignPath,

    [string]$ExpectedCampaignSha256 = '',

    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackageSealPath,

    [Parameter(Mandatory = $true)]
    [string]$SourceSetDirectory,

    [Parameter(Mandatory = $true)]
    [string]$SourceSetSealPath,

    [string]$OutputPath = ''
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

$campaignImport = Import-PBRemoteVisualStep22Campaign -Path $CampaignPath -ExpectedSha256 $ExpectedCampaignSha256
$campaign = $campaignImport.value
$resolvedPackage = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedSourceSet = [System.IO.Path]::GetFullPath($SourceSetDirectory).TrimEnd('\')
$resolvedSourceSetSeal = [System.IO.Path]::GetFullPath($SourceSetSealPath)

$packageVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
$packageVerificationText = @(& $packageVerifier -PackageDirectory $resolvedPackage `
    -PackageSealPath $resolvedPackageSeal -ExpectedManifestSha256 ([string]$campaign.runtime.packageManifestSha256)) -join "`n"
$packageVerification = ConvertFrom-PBStrictJsonText -Text $packageVerificationText -Name 'Step 22 package verification output'
Require-Condition -Condition ($packageVerification.verified -is [bool] -and [bool]$packageVerification.verified) `
    -Message 'Step 22 package verifier did not return a verified result'

$sourceVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
$sourceVerificationText = @(& $sourceVerifier -SourceSetDirectory $resolvedSourceSet `
    -SourceSealPath $resolvedSourceSetSeal -ExpectedManifestSha256 ([string]$campaign.sourceSet.manifestSha256)) -join "`n"
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

Require-Condition -Condition ([string]$packageManifestIdentity.sha256 -ceq [string]$campaign.runtime.packageManifestSha256 -and
    [string]$packageSealIdentity.sha256 -ceq [string]$campaign.runtime.packageSealSha256 -and
    [string]$packageManifest.buildIdentity.headCommit -ceq [string]$campaign.runtime.gitCommit -and
    [string]$packageManifest.buildIdentity.headTree -ceq [string]$campaign.runtime.headTree -and
    [string]$packageManifest.buildIdentityFingerprintSha256 -ceq [string]$campaign.runtime.buildIdentityFingerprintSha256 -and
    [string]$packageManifest.packagePayloadFingerprintSha256 -ceq [string]$campaign.runtime.packagePayloadFingerprintSha256) `
    -Message 'Step 22 campaign runtime identity differs from the verified package'

$packageApplications = @($packageManifest.applications)
foreach ($role in @('Encoder', 'Decoder'))
{
    $campaignApplications = @($campaign.runtime.applications | Where-Object { [string]$_.role -ceq $role })
    $manifestApplications = @($packageApplications | Where-Object { [string]$_.role -ceq $role })
    Require-Condition -Condition ($campaignApplications.Count -eq 1 -and $manifestApplications.Count -eq 1) `
        -Message "Step 22 package application role is not unique: $role"
    foreach ($name in @('role', 'application', 'relativeExecutablePath', 'size', 'sha256', 'versionOutput'))
    {
        Require-Condition -Condition ([string]($campaignApplications[0][$name]) -ceq [string]($manifestApplications[0][$name])) `
            -Message "Step 22 $role application $name differs from the verified package"
    }
    $executablePath = Join-Path $resolvedPackage ([string]$manifestApplications[0].relativeExecutablePath).Replace('/', '\')
    $executableIdentity = Get-PBFileIdentity -Path $executablePath
    Require-Condition -Condition ([UInt64]$executableIdentity.size -eq [UInt64]$manifestApplications[0].size -and
        [string]$executableIdentity.sha256 -ceq [string]$manifestApplications[0].sha256) `
        -Message "Step 22 $role executable differs from the package manifest"
}

Require-Condition -Condition ([string]$sourceManifestIdentity.sha256 -ceq [string]$campaign.sourceSet.manifestSha256 -and
    [string]$sourceSealIdentity.sha256 -ceq [string]$campaign.sourceSet.sealSha256 -and
    [string]$sourceManifest.sourceSetId -ceq [string]$campaign.sourceSet.sourceSetId -and
    [string]$sourceManifest.sourceSetFingerprintSha256 -ceq [string]$campaign.sourceSet.sourceSetFingerprintSha256) `
    -Message 'Step 22 campaign source-set identity differs from the verified source set'

$campaignSources = @($campaign.sourceSet.files)
$manifestSources = @($sourceManifest.files)
foreach ($campaignSource in $campaignSources)
{
    $matches = @($manifestSources | Where-Object { [string]$_.path -ceq [string]$campaignSource.relativePath })
    Require-Condition -Condition ($matches.Count -eq 1 -and
        [UInt64]$matches[0].size -eq [UInt64]$campaignSource.size -and
        [string]$matches[0].sha256 -ceq [string]$campaignSource.sha256 -and
        [string]$matches[0].pixelBridgeSegmentCompression -ceq 'RAW/OFF') `
        -Message "Step 22 source manifest binding failed: $($campaignSource.relativePath)"
    $sourcePath = Join-Path $resolvedSourceSet ([string]$campaignSource.relativePath)
    $sourceIdentity = Get-PBFileIdentity -Path $sourcePath
    Require-Condition -Condition ([UInt64]$sourceIdentity.size -eq [UInt64]$campaignSource.size -and
        [string]$sourceIdentity.sha256 -ceq [string]$campaignSource.sha256) `
        -Message "Step 22 source bytes differ from the campaign: $($campaignSource.relativePath)"
}
Require-Condition -Condition ([string]$sourceManifest.zipSourcePayload.archivePath -ceq [string]$campaign.sourceSet.zipSourcePayload.archiveRelativePath -and
    [string]$sourceManifest.zipSourcePayload.entryPath -ceq [string]$campaign.sourceSet.zipSourcePayload.entryPath -and
    [UInt64]$sourceManifest.zipSourcePayload.size -eq [UInt64]$campaign.sourceSet.zipSourcePayload.size -and
    [string]$sourceManifest.zipSourcePayload.sha256 -ceq [string]$campaign.sourceSet.zipSourcePayload.sha256) `
    -Message 'Step 22 inner ZIP payload binding differs from the sealed source set'

$result = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep22CampaignVerification.1'
    verified = $true
    status = 'READY_NOT_EXECUTED'
    campaignId = [string]$campaign.campaignId
    runCount = [UInt32]@($campaign.runs).Count
    runtimeCommit = [string]$campaign.runtime.gitCommit
    sourceSetId = [string]$campaign.sourceSet.sourceSetId
    remoteVisualSmokePass = $false
    certifiedRemoteVisualProfile = $false
    artifacts = [ordered]@{
        campaign = $campaignImport.identity
        packageManifest = $packageManifestIdentity
        packageSeal = $packageSealIdentity
        sourceManifest = $sourceManifestIdentity
        sourceSeal = $sourceSealIdentity
    }
}
if (-not [string]::IsNullOrWhiteSpace($OutputPath))
{
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
    if ((Test-IsWithin -Path $resolvedOutput -Root $resolvedPackage) -or
        (Test-IsWithin -Path $resolvedOutput -Root $resolvedSourceSet))
    {
        throw 'Step 22 verification output must not mutate the immutable package or source set'
    }
    [void](Write-PBCreateOnlyJson -Path $resolvedOutput -Value $result -Depth 16)
}
$result | ConvertTo-Json -Depth 16

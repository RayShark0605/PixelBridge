[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$PackageSealPath,

    [Parameter(Mandatory = $true)]
    [string]$SourceManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$SourceSealPath,

    [Parameter(Mandatory = $true)]
    [string]$RemoteMetadataPath,

    [Parameter(Mandatory = $true)]
    [string]$EncoderEnvironmentPath,

    [Parameter(Mandatory = $true)]
    [string]$DecoderEnvironmentPath,

    [Parameter(Mandatory = $true)]
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

function Get-FileSha256
{
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextSha256
{
    param([Parameter(Mandatory = $true)][string]$Text)
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally
    {
        $algorithm.Dispose()
    }
}

function Read-BoundedJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [UInt64]$MaximumBytes
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        throw "JSON artifact does not exist: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or $item.Length -eq 0 -or
        [UInt64]$item.Length -gt $MaximumBytes)
    {
        throw "JSON artifact is empty, oversized, or a reparse point: $Path"
    }
    try
    {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    }
    catch
    {
        throw "JSON artifact is malformed: $Path ($($_.Exception.Message))"
    }
}

function Get-ArtifactIdentity
{
    param([Parameter(Mandatory = $true)][string]$Path)
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $item = Get-Item -LiteralPath $resolvedPath
    return [ordered]@{
        path = $resolvedPath
        size = [UInt64]$item.Length
        sha256 = Get-FileSha256 -Path $resolvedPath
    }
}

function Get-EnvironmentFingerprint
{
    param([Parameter(Mandatory = $true)][object]$Environment)
    $identity = [ordered]@{}
    foreach ($entry in $Environment.GetEnumerator())
    {
        if ($entry.Key -cne 'environmentFingerprintSha256')
        {
            $identity.Add([string]$entry.Key, $entry.Value)
        }
    }
    return Get-TextSha256 -Text ($identity | ConvertTo-Json -Depth 40 -Compress)
}

$resolvedPackageManifest = [System.IO.Path]::GetFullPath($PackageManifestPath)
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedSourceManifest = [System.IO.Path]::GetFullPath($SourceManifestPath)
$resolvedSourceSeal = [System.IO.Path]::GetFullPath($SourceSealPath)
$resolvedRemoteMetadata = [System.IO.Path]::GetFullPath($RemoteMetadataPath)
$resolvedEncoderEnvironment = [System.IO.Path]::GetFullPath($EncoderEnvironmentPath)
$resolvedDecoderEnvironment = [System.IO.Path]::GetFullPath($DecoderEnvironmentPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryOutput = "$resolvedOutput.partial"
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
{
    throw "Create-only deployment manifest or partial already exists: $resolvedOutput"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Deployment output parent does not exist: $outputParent"
}

$packageVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
$sourceVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
$deploymentVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualDeploymentManifest.ps1'
foreach ($verifier in @($packageVerifier, $sourceVerifier, $deploymentVerifier))
{
    if (-not (Test-Path -LiteralPath $verifier -PathType Leaf))
    {
        throw "Required independent verifier is missing: $verifier"
    }
}
$null = & $packageVerifier -PackageDirectory ([System.IO.Path]::GetDirectoryName($resolvedPackageManifest)) -PackageSealPath $resolvedPackageSeal
$null = & $sourceVerifier -SourceSetDirectory ([System.IO.Path]::GetDirectoryName($resolvedSourceManifest)) -SourceSealPath $resolvedSourceSeal

$packageManifest = Read-BoundedJson -Path $resolvedPackageManifest -MaximumBytes 32MB
$sourceManifest = Read-BoundedJson -Path $resolvedSourceManifest -MaximumBytes 1MB
$remoteMetadata = Read-BoundedJson -Path $resolvedRemoteMetadata -MaximumBytes 64KB
$encoderEnvironment = Read-BoundedJson -Path $resolvedEncoderEnvironment -MaximumBytes 2MB
$decoderEnvironment = Read-BoundedJson -Path $resolvedDecoderEnvironment -MaximumBytes 2MB
if ($packageManifest.schema -cne 'PixelBridge.PortablePackage.2' -or
    $sourceManifest.schema -cne 'PixelBridge.RemoteVisualSourceSet.2' -or
    $remoteMetadata.schema -cne 'PixelBridge.RemoteVisualRunMetadata.1' -or
    $encoderEnvironment.schema -cne 'PixelBridge.EndpointEnvironment.2' -or
    $decoderEnvironment.schema -cne 'PixelBridge.EndpointEnvironment.2')
{
    throw 'Deployment input schema mismatch'
}
$runId = [string]$remoteMetadata.runId
if ($runId -cnotmatch '^[0-9a-f]{32}$' -or $remoteMetadata.channelType -cne 'RemoteVisual' -or
    $encoderEnvironment.endpointRole -cne 'Encoder' -or $decoderEnvironment.endpointRole -cne 'Decoder' -or
    $encoderEnvironment.runId -cne $runId -or $decoderEnvironment.runId -cne $runId)
{
    throw 'Deployment input RunId or endpoint roles are inconsistent'
}
$roles = @($packageManifest.endpointRoles)
if ($roles.Count -ne 2 -or -not ($roles -ccontains 'Encoder') -or -not ($roles -ccontains 'Decoder'))
{
    throw 'Deployment requires one shared Both-role package'
}
foreach ($environment in @($encoderEnvironment, $decoderEnvironment))
{
    if ($environment.environmentFingerprintSha256 -cnotmatch '^[0-9a-f]{64}$' -or
        (Get-EnvironmentFingerprint -Environment $environment) -cne $environment.environmentFingerprintSha256)
    {
        throw "$($environment.endpointRole) environment self-fingerprint is invalid"
    }
}

$packageManifestIdentity = Get-ArtifactIdentity -Path $resolvedPackageManifest
$packageSealIdentity = Get-ArtifactIdentity -Path $resolvedPackageSeal
$sourceManifestIdentity = Get-ArtifactIdentity -Path $resolvedSourceManifest
$sourceSealIdentity = Get-ArtifactIdentity -Path $resolvedSourceSeal
$remoteMetadataIdentity = Get-ArtifactIdentity -Path $resolvedRemoteMetadata
$encoderEnvironmentIdentity = Get-ArtifactIdentity -Path $resolvedEncoderEnvironment
$decoderEnvironmentIdentity = Get-ArtifactIdentity -Path $resolvedDecoderEnvironment
$logicalIdentity = [ordered]@{
    runId = $runId
    package = [ordered]@{
        manifestSha256 = $packageManifestIdentity.sha256
        sealSha256 = $packageSealIdentity.sha256
        packageName = [string]$packageManifest.packageName
        endpointRoles = @('Encoder', 'Decoder')
        headCommit = [string]$packageManifest.buildIdentity.headCommit
        headTree = [string]$packageManifest.buildIdentity.headTree
        buildIdentityFingerprintSha256 = [string]$packageManifest.buildIdentityFingerprintSha256
        testedSourceFingerprintSha256 = [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256
        packagePayloadFingerprintSha256 = [string]$packageManifest.packagePayloadFingerprintSha256
    }
    sourceSet = [ordered]@{
        manifestSha256 = $sourceManifestIdentity.sha256
        sealSha256 = $sourceSealIdentity.sha256
        sourceSetId = [string]$sourceManifest.sourceSetId
        sourceSetFingerprintSha256 = [string]$sourceManifest.sourceSetFingerprintSha256
    }
    remoteMetadata = [ordered]@{
        sha256 = $remoteMetadataIdentity.sha256
        runId = $runId
        remoteProvider = [string]$remoteMetadata.remoteProvider
        remoteProviderVersion = [string]$remoteMetadata.remoteProviderVersion
        remoteMode = [string]$remoteMetadata.remoteMode
        targetFps = if ($null -eq $remoteMetadata.targetFps) { $null } else { [double]$remoteMetadata.targetFps }
        chromaMode = [string]$remoteMetadata.chromaMode
    }
    endpoints = @(
        [ordered]@{
            role = 'Encoder'
            environmentArtifactSha256 = $encoderEnvironmentIdentity.sha256
            environmentFingerprintSha256 = [string]$encoderEnvironment.environmentFingerprintSha256
        },
        [ordered]@{
            role = 'Decoder'
            environmentArtifactSha256 = $decoderEnvironmentIdentity.sha256
            environmentFingerprintSha256 = [string]$decoderEnvironment.environmentFingerprintSha256
        }
    )
}
$deploymentFingerprint = Get-TextSha256 -Text ($logicalIdentity | ConvertTo-Json -Depth 40 -Compress)
$deployment = [ordered]@{
    schema = 'PixelBridge.RemoteVisualDeployment.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    runId = $runId
    deploymentFingerprintSha256 = $deploymentFingerprint
    logicalIdentity = $logicalIdentity
    artifacts = [ordered]@{
        packageManifest = $packageManifestIdentity
        packageSeal = $packageSealIdentity
        sourceManifest = $sourceManifestIdentity
        sourceSeal = $sourceSealIdentity
        remoteMetadata = $remoteMetadataIdentity
        endpointEnvironments = @(
            [ordered]@{
                role = 'Encoder'
                artifact = $encoderEnvironmentIdentity
                environmentFingerprintSha256 = [string]$encoderEnvironment.environmentFingerprintSha256
            },
            [ordered]@{
                role = 'Decoder'
                artifact = $decoderEnvironmentIdentity
                environmentFingerprintSha256 = [string]$decoderEnvironment.environmentFingerprintSha256
            }
        )
    }
}

$published = $false
try
{
    Write-NewUtf8File -Path $temporaryOutput -Content ($deployment | ConvertTo-Json -Depth 40)
    $null = & $deploymentVerifier -DeploymentManifestPath $temporaryOutput
    Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
    $verification = & $deploymentVerifier -DeploymentManifestPath $resolvedOutput
    $published = $true
    $outputItem = Get-Item -LiteralPath $resolvedOutput
    [ordered]@{
        deploymentManifestPath = $resolvedOutput
        size = [UInt64]$outputItem.Length
        sha256 = Get-FileSha256 -Path $resolvedOutput
        runId = $runId
        deploymentFingerprintSha256 = $deploymentFingerprint
        verification = $verification
    } | ConvertTo-Json -Depth 12
}
finally
{
    if (-not $published)
    {
        foreach ($path in @($temporaryOutput, $resolvedOutput))
        {
            if (Test-Path -LiteralPath $path -PathType Leaf)
            {
                [System.IO.File]::Delete($path)
            }
        }
    }
}

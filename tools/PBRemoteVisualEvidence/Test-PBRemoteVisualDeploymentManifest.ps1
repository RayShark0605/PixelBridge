[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DeploymentManifestPath,

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

function Require-Hash
{
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Value -isnot [string] -or $Value -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "$Name must be 64 lowercase hexadecimal characters"
    }
}

function Assert-ArtifactIdentity
{
    param(
        [Parameter(Mandatory = $true)][object]$Identity,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Identity.path -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$Identity.path))
    {
        throw "$Name path is missing"
    }
    Require-Hash -Value $Identity.sha256 -Name "$Name SHA-256"
    $resolvedPath = [System.IO.Path]::GetFullPath([string]$Identity.path)
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
    {
        throw "$Name artifact does not exist: $resolvedPath"
    }
    $item = Get-Item -LiteralPath $resolvedPath
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        [UInt64]$item.Length -ne [UInt64]$Identity.size -or
        (Get-FileSha256 -Path $resolvedPath) -cne $Identity.sha256)
    {
        throw "$Name artifact does not match its exact identity"
    }
    return $resolvedPath
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

function Assert-BindingIdentity
{
    param(
        [Parameter(Mandatory = $true)][object]$Binding,
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ([UInt64]$Binding.size -ne [UInt64]$Expected.size -or $Binding.sha256 -cne $Expected.sha256)
    {
        throw "$Name binding does not match the deployment artifact"
    }
}

$resolvedDeploymentManifest = [System.IO.Path]::GetFullPath($DeploymentManifestPath)
$deployment = Read-BoundedJson -Path $resolvedDeploymentManifest -MaximumBytes 2MB
if ($deployment.schema -cne 'PixelBridge.RemoteVisualDeployment.1' -or
    $deployment.runId -cnotmatch '^[0-9a-f]{32}$')
{
    throw 'Deployment schema or RunId is invalid'
}
Require-Hash -Value $deployment.deploymentFingerprintSha256 -Name 'deploymentFingerprintSha256'
$computedDeploymentFingerprint = Get-TextSha256 -Text ($deployment.logicalIdentity | ConvertTo-Json -Depth 40 -Compress)
if ($computedDeploymentFingerprint -cne $deployment.deploymentFingerprintSha256)
{
    throw 'Deployment fingerprint does not match logicalIdentity'
}

$artifacts = $deployment.artifacts
$packageManifestPath = Assert-ArtifactIdentity -Identity $artifacts.packageManifest -Name 'package manifest'
$packageSealPath = Assert-ArtifactIdentity -Identity $artifacts.packageSeal -Name 'package seal'
$sourceManifestPath = Assert-ArtifactIdentity -Identity $artifacts.sourceManifest -Name 'source manifest'
$sourceSealPath = Assert-ArtifactIdentity -Identity $artifacts.sourceSeal -Name 'source seal'
$remoteMetadataPath = Assert-ArtifactIdentity -Identity $artifacts.remoteMetadata -Name 'RemoteVisual metadata'
if ([System.IO.Path]::GetFileName($packageManifestPath) -cne 'package-manifest.json' -or
    [System.IO.Path]::GetFileName($sourceManifestPath) -cne 'source-manifest.json')
{
    throw 'Deployment references non-canonical package/source manifest names'
}
$packageVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
$sourceVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
$null = & $packageVerifier -PackageDirectory ([System.IO.Path]::GetDirectoryName($packageManifestPath)) -PackageSealPath $packageSealPath
$null = & $sourceVerifier -SourceSetDirectory ([System.IO.Path]::GetDirectoryName($sourceManifestPath)) -SourceSealPath $sourceSealPath

$packageManifest = Read-BoundedJson -Path $packageManifestPath -MaximumBytes 32MB
$sourceManifest = Read-BoundedJson -Path $sourceManifestPath -MaximumBytes 1MB
$remoteMetadata = Read-BoundedJson -Path $remoteMetadataPath -MaximumBytes 64KB
if ($packageManifest.schema -cne 'PixelBridge.PortablePackage.2' -or
    $sourceManifest.schema -cne 'PixelBridge.RemoteVisualSourceSet.2' -or
    $remoteMetadata.schema -cne 'PixelBridge.RemoteVisualRunMetadata.1' -or
    $remoteMetadata.runId -cne $deployment.runId)
{
    throw 'Deployment artifacts have incompatible schemas or RunId'
}
$roles = @($packageManifest.endpointRoles)
if ($roles.Count -ne 2 -or -not ($roles -ccontains 'Encoder') -or -not ($roles -ccontains 'Decoder'))
{
    throw 'Deployment must use one shared Both-role package'
}

$logical = $deployment.logicalIdentity
foreach ($value in @($logical.package.manifestSha256, $logical.package.sealSha256,
    $logical.package.buildIdentityFingerprintSha256, $logical.package.testedSourceFingerprintSha256,
    $logical.package.packagePayloadFingerprintSha256, $logical.sourceSet.manifestSha256,
    $logical.sourceSet.sealSha256, $logical.sourceSet.sourceSetFingerprintSha256,
    $logical.remoteMetadata.sha256))
{
    Require-Hash -Value $value -Name 'logical identity SHA-256'
}
if ($logical.runId -cne $deployment.runId -or $logical.remoteMetadata.runId -cne $deployment.runId -or
    $logical.package.manifestSha256 -cne $artifacts.packageManifest.sha256 -or
    $logical.package.sealSha256 -cne $artifacts.packageSeal.sha256 -or
    $logical.package.packageName -cne $packageManifest.packageName -or
    $logical.package.buildIdentityFingerprintSha256 -cne $packageManifest.buildIdentityFingerprintSha256 -or
    $logical.package.testedSourceFingerprintSha256 -cne $packageManifest.buildIdentity.testedSourceFingerprintSha256 -or
    $logical.package.packagePayloadFingerprintSha256 -cne $packageManifest.packagePayloadFingerprintSha256 -or
    $logical.package.headCommit -cne $packageManifest.buildIdentity.headCommit -or
    $logical.package.headTree -cne $packageManifest.buildIdentity.headTree -or
    $logical.sourceSet.manifestSha256 -cne $artifacts.sourceManifest.sha256 -or
    $logical.sourceSet.sealSha256 -cne $artifacts.sourceSeal.sha256 -or
    $logical.sourceSet.sourceSetId -cne $sourceManifest.sourceSetId -or
    $logical.sourceSet.sourceSetFingerprintSha256 -cne $sourceManifest.sourceSetFingerprintSha256 -or
    $logical.remoteMetadata.sha256 -cne $artifacts.remoteMetadata.sha256 -or
    $logical.remoteMetadata.remoteProvider -cne $remoteMetadata.remoteProvider -or
    $logical.remoteMetadata.remoteProviderVersion -cne $remoteMetadata.remoteProviderVersion -or
    $logical.remoteMetadata.remoteMode -cne $remoteMetadata.remoteMode)
{
    throw 'Logical deployment identity conflicts with package/source/metadata artifacts'
}

$endpointArtifacts = @($artifacts.endpointEnvironments)
$logicalEndpoints = @($logical.endpoints)
if ($endpointArtifacts.Count -ne 2 -or $logicalEndpoints.Count -ne 2)
{
    throw 'Deployment must bind exactly two endpoint environments'
}
$seenRoles = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($endpointArtifact in $endpointArtifacts)
{
    $role = [string]$endpointArtifact.role
    if (($role -cne 'Encoder' -and $role -cne 'Decoder') -or -not $seenRoles.Add($role))
    {
        throw "Invalid or duplicate endpoint role: $role"
    }
    Require-Hash -Value $endpointArtifact.environmentFingerprintSha256 -Name "$role environment fingerprint"
    $environmentPath = Assert-ArtifactIdentity -Identity $endpointArtifact.artifact -Name "$role environment"
    $environment = Read-BoundedJson -Path $environmentPath -MaximumBytes 2MB
    if ($environment.schema -cne 'PixelBridge.EndpointEnvironment.2' -or
        $environment.endpointRole -cne $role -or $environment.runId -cne $deployment.runId -or
        $environment.environmentFingerprintSha256 -cne $endpointArtifact.environmentFingerprintSha256 -or
        (Get-EnvironmentFingerprint -Environment $environment) -cne $environment.environmentFingerprintSha256)
    {
        throw "$role environment schema, role, RunId, or self-fingerprint is invalid"
    }
    Assert-BindingIdentity -Binding $environment.artifactBindings.packageManifest -Expected $artifacts.packageManifest -Name "$role package manifest"
    Assert-BindingIdentity -Binding $environment.artifactBindings.packageSeal -Expected $artifacts.packageSeal -Name "$role package seal"
    Assert-BindingIdentity -Binding $environment.artifactBindings.sourceManifest -Expected $artifacts.sourceManifest -Name "$role source manifest"
    Assert-BindingIdentity -Binding $environment.artifactBindings.sourceSeal -Expected $artifacts.sourceSeal -Name "$role source seal"
    Assert-BindingIdentity -Binding $environment.artifactBindings.remoteMetadata -Expected $artifacts.remoteMetadata -Name "$role metadata"
    if ($environment.artifactBindings.packageName -cne $packageManifest.packageName -or
        $environment.artifactBindings.packagePayloadFingerprintSha256 -cne $packageManifest.packagePayloadFingerprintSha256 -or
        $environment.artifactBindings.buildIdentityFingerprintSha256 -cne $packageManifest.buildIdentityFingerprintSha256 -or
        $environment.artifactBindings.testedSourceFingerprintSha256 -cne $packageManifest.buildIdentity.testedSourceFingerprintSha256 -or
        $environment.artifactBindings.sourceSetId -cne $sourceManifest.sourceSetId -or
        $environment.artifactBindings.sourceSetFingerprintSha256 -cne $sourceManifest.sourceSetFingerprintSha256)
    {
        throw "$role environment logical package/source binding is inconsistent"
    }
    $matchingLogicalEndpoint = @($logicalEndpoints | Where-Object { $_.role -ceq $role })
    if ($matchingLogicalEndpoint.Count -ne 1 -or
        $matchingLogicalEndpoint[0].environmentArtifactSha256 -cne $endpointArtifact.artifact.sha256 -or
        $matchingLogicalEndpoint[0].environmentFingerprintSha256 -cne $environment.environmentFingerprintSha256)
    {
        throw "$role logical endpoint identity does not match its environment artifact"
    }
}
if (-not $seenRoles.Contains('Encoder') -or -not $seenRoles.Contains('Decoder'))
{
    throw 'Deployment is missing Encoder or Decoder environment evidence'
}

$deploymentItem = Get-Item -LiteralPath $resolvedDeploymentManifest
$result = [ordered]@{
    schema = 'PixelBridge.RemoteVisualDeploymentVerification.1'
    verified = $true
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    runId = $deployment.runId
    deploymentManifest = [ordered]@{
        path = $resolvedDeploymentManifest
        size = [UInt64]$deploymentItem.Length
        sha256 = Get-FileSha256 -Path $resolvedDeploymentManifest
    }
    deploymentFingerprintSha256 = $deployment.deploymentFingerprintSha256
    packageManifestSha256 = $artifacts.packageManifest.sha256
    sourceManifestSha256 = $artifacts.sourceManifest.sha256
    remoteMetadataSha256 = $artifacts.remoteMetadata.sha256
    endpointRoles = @('Encoder', 'Decoder')
}
$json = $result | ConvertTo-Json -Depth 10
if ($OutputPath)
{
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
    $temporaryOutput = "$resolvedOutput.partial"
    if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
    {
        throw "Create-only verification output or partial already exists: $resolvedOutput"
    }
    $outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
    if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
    {
        throw "Verification output parent does not exist: $outputParent"
    }
    try
    {
        Write-NewUtf8File -Path $temporaryOutput -Content ($json + "`n")
        Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
        {
            [System.IO.File]::Delete($temporaryOutput)
        }
    }
}
$json

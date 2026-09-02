[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Encoder', 'Decoder')]
    [string]$EndpointRole,

    [Parameter(Mandatory = $true)]
    [string]$PixelBridgeDecoderPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

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

    [string]$RunId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-MonitorId
{
    param([UInt16[]]$Value)
    if ($null -eq $Value)
    {
        return $null
    }
    return -join ($Value | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
}

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
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = [System.IO.Path]::GetFullPath($Path)
        size = [UInt64]$item.Length
        sha256 = Get-FileSha256 -Path $Path
    }
}

$decoderPath = [System.IO.Path]::GetFullPath($PixelBridgeDecoderPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryOutput = "$resolvedOutput.partial"
$resolvedPackageManifest = [System.IO.Path]::GetFullPath($PackageManifestPath)
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedSourceManifest = [System.IO.Path]::GetFullPath($SourceManifestPath)
$resolvedSourceSeal = [System.IO.Path]::GetFullPath($SourceSealPath)
$resolvedRemoteMetadata = [System.IO.Path]::GetFullPath($RemoteMetadataPath)
if (-not (Test-Path -LiteralPath $decoderPath -PathType Leaf))
{
    throw "PixelBridgeDecoder does not exist: $decoderPath"
}
if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
{
    throw "Create-only environment output or partial already exists: $resolvedOutput"
}
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container))
{
    throw "Environment output parent does not exist: $outputParent"
}
if ($RunId -and $RunId -cnotmatch '^[0-9a-f]{32}$')
{
    throw 'RunId must be empty or 32 lowercase hexadecimal characters'
}
if ([System.IO.Path]::GetFileName($resolvedPackageManifest) -cne 'package-manifest.json' -or
    [System.IO.Path]::GetFileName($resolvedSourceManifest) -cne 'source-manifest.json')
{
    throw 'Package/source manifest names must be canonical'
}

$packageDirectory = [System.IO.Path]::GetDirectoryName($resolvedPackageManifest)
$sourceSetDirectory = [System.IO.Path]::GetDirectoryName($resolvedSourceManifest)
$packageVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
$sourceVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
if (-not (Test-Path -LiteralPath $packageVerifier -PathType Leaf) -or
    -not (Test-Path -LiteralPath $sourceVerifier -PathType Leaf))
{
    throw 'Required independent package/source verifier is missing'
}
$packageVerification = & $packageVerifier -PackageDirectory $packageDirectory -PackageSealPath $resolvedPackageSeal
$sourceVerification = & $sourceVerifier -SourceSetDirectory $sourceSetDirectory -SourceSealPath $resolvedSourceSeal

$packageManifest = Read-BoundedJson -Path $resolvedPackageManifest -MaximumBytes 32MB
$packageSeal = Read-BoundedJson -Path $resolvedPackageSeal -MaximumBytes 1MB
$sourceManifest = Read-BoundedJson -Path $resolvedSourceManifest -MaximumBytes 1MB
$sourceSeal = Read-BoundedJson -Path $resolvedSourceSeal -MaximumBytes 1MB
$remoteMetadata = Read-BoundedJson -Path $resolvedRemoteMetadata -MaximumBytes 64KB
if ($packageManifest.schema -cne 'PixelBridge.PortablePackage.2' -or
    $packageSeal.schema -cne 'PixelBridge.PortablePackageSeal.2' -or
    $sourceManifest.schema -cne 'PixelBridge.RemoteVisualSourceSet.2' -or
    $sourceSeal.schema -cne 'PixelBridge.RemoteVisualSourceSetSeal.2' -or
    $remoteMetadata.schema -cne 'PixelBridge.RemoteVisualRunMetadata.1')
{
    throw 'Package/source/metadata schema mismatch'
}
if ($remoteMetadata.runId -cnotmatch '^[0-9a-f]{32}$' -or $remoteMetadata.channelType -cne 'RemoteVisual' -or
    [string]::IsNullOrWhiteSpace([string]$remoteMetadata.remoteProvider))
{
    throw 'RemoteVisual metadata identity is incomplete or invalid'
}
if ($RunId -and $RunId -cne $remoteMetadata.runId)
{
    throw 'Explicit RunId conflicts with the shared RemoteVisual metadata preset'
}
$effectiveRunId = [string]$remoteMetadata.runId
$endpointRoles = @($packageManifest.endpointRoles)
if ($endpointRoles.Count -ne 2 -or -not ($endpointRoles -ccontains 'Encoder') -or
    -not ($endpointRoles -ccontains 'Decoder'))
{
    throw 'Endpoint evidence requires one shared Both-role portable package'
}
$decoderApplications = @($packageManifest.applications | Where-Object {
    $_.role -ceq 'Decoder' -and $_.application -ceq 'PixelBridgeDecoder'
})
if ($decoderApplications.Count -ne 1)
{
    throw 'Shared portable package has no unique Decoder application binding'
}
$decoderApplication = $decoderApplications[0]
$expectedDecoderPath = [System.IO.Path]::GetFullPath((Join-Path $packageDirectory ([string]$decoderApplication.relativeExecutablePath).Replace('/', '\')))
if (-not $decoderPath.Equals($expectedDecoderPath, [StringComparison]::OrdinalIgnoreCase))
{
    throw 'Monitor probe must use the Decoder executable inside the verified shared package'
}
$decoderItem = Get-Item -LiteralPath $decoderPath
$decoderSha256 = Get-FileSha256 -Path $decoderPath
if ([UInt64]$decoderItem.Length -ne [UInt64]$decoderApplication.size -or
    $decoderSha256 -cne $decoderApplication.sha256)
{
    throw 'Packaged Decoder executable identity does not match the package manifest'
}

$monitorJson = & $decoderPath --list-monitors
if ($LASTEXITCODE -ne 0)
{
    throw "PixelBridge monitor catalog failed with exit code $LASTEXITCODE"
}
$monitorJsonText = $monitorJson -join "`n"
if ([System.Text.UTF8Encoding]::new($false).GetByteCount($monitorJsonText) -gt 1MB)
{
    throw 'PixelBridge monitor catalog exceeds the 1 MiB environment bound'
}
$monitorCatalog = $monitorJsonText | ConvertFrom-Json -AsHashtable -Depth 100
if ($monitorCatalog.schema -cne 'PixelBridge.MonitorCatalog.1')
{
    throw 'PixelBridge monitor catalog schema mismatch'
}
$versionOutput = & $decoderPath --version
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($versionOutput -join "`n")))
{
    throw "Packaged PixelBridgeDecoder --version failed with exit code $LASTEXITCODE"
}

$operatingSystem = Get-CimInstance Win32_OperatingSystem
$computerSystem = Get-CimInstance Win32_ComputerSystem
$processors = @(Get-CimInstance Win32_Processor | Sort-Object -Property DeviceID | ForEach-Object {
    [ordered]@{
        deviceId = $_.DeviceID
        name = $_.Name
        manufacturer = $_.Manufacturer
        cores = [UInt32]$_.NumberOfCores
        logicalProcessors = [UInt32]$_.NumberOfLogicalProcessors
    }
})
$graphicsAdapters = @(Get-CimInstance Win32_VideoController | Sort-Object -Property PNPDeviceID | ForEach-Object {
    [ordered]@{
        name = $_.Name
        driverVersion = $_.DriverVersion
        pnpDeviceId = $_.PNPDeviceID
        adapterRam = if ($null -eq $_.AdapterRAM) { $null } else { [UInt64]$_.AdapterRAM }
        currentHorizontalResolution = $_.CurrentHorizontalResolution
        currentVerticalResolution = $_.CurrentVerticalResolution
        currentRefreshRate = $_.CurrentRefreshRate
    }
})
if ($processors.Count -gt 256 -or $graphicsAdapters.Count -gt 256)
{
    throw 'Processor or graphics-adapter inventory exceeds the 256-entry environment bound'
}
$displayEdid = @()
try
{
    $displayEdid = @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID | Sort-Object -Property InstanceName | ForEach-Object {
        [ordered]@{
            instanceName = $_.InstanceName
            manufacturer = Convert-MonitorId $_.ManufacturerName
            productCode = Convert-MonitorId $_.ProductCodeID
            serialNumber = Convert-MonitorId $_.SerialNumberID
            userFriendlyName = Convert-MonitorId $_.UserFriendlyName
            active = [bool]$_.Active
        }
    })
}
catch
{
    $displayEdid = @([ordered]@{ unavailableReason = $_.Exception.Message })
}
if ($displayEdid.Count -gt 256)
{
    throw 'Display EDID inventory exceeds the 256-entry environment bound'
}
$windowsVersionKey = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$artifactBindings = [ordered]@{
    packageManifest = Get-ArtifactIdentity -Path $resolvedPackageManifest
    packageSeal = Get-ArtifactIdentity -Path $resolvedPackageSeal
    packageName = [string]$packageManifest.packageName
    packagePayloadFingerprintSha256 = [string]$packageManifest.packagePayloadFingerprintSha256
    buildIdentityFingerprintSha256 = [string]$packageManifest.buildIdentityFingerprintSha256
    testedSourceFingerprintSha256 = [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256
    sourceManifest = Get-ArtifactIdentity -Path $resolvedSourceManifest
    sourceSeal = Get-ArtifactIdentity -Path $resolvedSourceSeal
    sourceSetId = [string]$sourceManifest.sourceSetId
    sourceSetFingerprintSha256 = [string]$sourceManifest.sourceSetFingerprintSha256
    remoteMetadata = Get-ArtifactIdentity -Path $resolvedRemoteMetadata
}

$fingerprint = [ordered]@{
    schema = 'PixelBridge.EndpointEnvironment.2'
    collectedUtc = [DateTime]::UtcNow.ToString('o')
    endpointRole = $EndpointRole
    runId = $effectiveRunId
    collectionSemantics = 'Read-only metadata; no screen pixels, screenshots, input hooks, display changes, or private remote-provider API'
    artifactBindings = $artifactBindings
    remoteProviderMetadata = [ordered]@{
        remoteProvider = [string]$remoteMetadata.remoteProvider
        remoteProviderVersion = [string]$remoteMetadata.remoteProviderVersion
        remoteMode = [string]$remoteMetadata.remoteMode
        targetFps = if ($null -eq $remoteMetadata.targetFps) { $null } else { [double]$remoteMetadata.targetFps }
        chromaMode = [string]$remoteMetadata.chromaMode
    }
    operatingSystem = [ordered]@{
        caption = $operatingSystem.Caption
        version = $operatingSystem.Version
        buildNumber = $operatingSystem.BuildNumber
        displayVersion = $windowsVersionKey.DisplayVersion
        ubr = $windowsVersionKey.UBR
        architecture = $operatingSystem.OSArchitecture
    }
    computer = [ordered]@{
        manufacturer = $computerSystem.Manufacturer
        model = $computerSystem.Model
        totalPhysicalMemory = [UInt64]$computerSystem.TotalPhysicalMemory
    }
    processors = $processors
    graphicsAdapters = $graphicsAdapters
    monitorCatalog = $monitorCatalog
    displayEdid = $displayEdid
    pixelBridgeDecoder = [ordered]@{
        path = $decoderPath
        size = [UInt64]$decoderItem.Length
        sha256 = $decoderSha256
        versionOutput = ($versionOutput -join "`n")
    }
    packageVerification = $packageVerification | ConvertFrom-Json -AsHashtable -Depth 40
    sourceSetVerification = $sourceVerification | ConvertFrom-Json -AsHashtable -Depth 40
}
$serializedFingerprint = $fingerprint | ConvertTo-Json -Depth 40
$normalizedFingerprint = $serializedFingerprint | ConvertFrom-Json -AsHashtable -Depth 100
$environmentFingerprint = Get-TextSha256 -Text ($normalizedFingerprint | ConvertTo-Json -Depth 40 -Compress)
$fingerprint.environmentFingerprintSha256 = $environmentFingerprint
$fingerprintJson = $fingerprint | ConvertTo-Json -Depth 40
if ([System.Text.UTF8Encoding]::new($false).GetByteCount($fingerprintJson) -gt 2MB)
{
    throw 'Endpoint environment artifact exceeds the 2 MiB evidence bound'
}
try
{
    Write-NewUtf8File -Path $temporaryOutput -Content $fingerprintJson
    Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
}
finally
{
    if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
    {
        [System.IO.File]::Delete($temporaryOutput)
    }
}
$fingerprint | ConvertTo-Json -Depth 40

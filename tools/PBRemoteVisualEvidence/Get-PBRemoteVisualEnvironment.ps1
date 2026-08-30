[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Encoder', 'Decoder')]
    [string]$EndpointRole,

    [Parameter(Mandatory = $true)]
    [string]$PixelBridgeDecoderPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$PackageManifestPath,

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
    param([string]$Path, [string]$Content)
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

$decoderPath = [System.IO.Path]::GetFullPath($PixelBridgeDecoderPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
if (-not (Test-Path -LiteralPath $decoderPath -PathType Leaf))
{
    throw "PixelBridgeDecoder does not exist: $decoderPath"
}
if (Test-Path -LiteralPath $resolvedOutput)
{
    throw "Create-only environment output already exists: $resolvedOutput"
}
if ($RunId -and $RunId -notmatch '^[0-9a-f]{32}$')
{
    throw 'RunId must be empty or 32 lowercase hexadecimal characters'
}

$monitorJson = & $decoderPath --list-monitors
if ($LASTEXITCODE -ne 0)
{
    throw "PixelBridge monitor catalog failed with exit code $LASTEXITCODE"
}
$monitorCatalog = $monitorJson | ConvertFrom-Json
if ($monitorCatalog.schema -ne 'PixelBridge.MonitorCatalog.1')
{
    throw 'PixelBridge monitor catalog schema mismatch'
}

$operatingSystem = Get-CimInstance Win32_OperatingSystem
$computerSystem = Get-CimInstance Win32_ComputerSystem
$processor = @(Get-CimInstance Win32_Processor | ForEach-Object {
    [ordered]@{
        name = $_.Name
        manufacturer = $_.Manufacturer
        cores = [UInt32]$_.NumberOfCores
        logicalProcessors = [UInt32]$_.NumberOfLogicalProcessors
    }
})
$graphicsAdapters = @(Get-CimInstance Win32_VideoController | ForEach-Object {
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
$displayEdid = @()
try
{
    $displayEdid = @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID | ForEach-Object {
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
$windowsVersionKey = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$decoderItem = Get-Item -LiteralPath $decoderPath
$packageSeal = $null
if ($PackageManifestPath)
{
    $resolvedPackageManifest = [System.IO.Path]::GetFullPath($PackageManifestPath)
    if (-not (Test-Path -LiteralPath $resolvedPackageManifest -PathType Leaf))
    {
        throw "Package manifest does not exist: $resolvedPackageManifest"
    }
    $packageItem = Get-Item -LiteralPath $resolvedPackageManifest
    $packageSeal = [ordered]@{
        path = $resolvedPackageManifest
        size = [UInt64]$packageItem.Length
        sha256 = (Get-FileHash -LiteralPath $resolvedPackageManifest -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$fingerprint = [ordered]@{
    schema = 'PixelBridge.EndpointEnvironment.1'
    collectedUtc = [DateTime]::UtcNow.ToString('o')
    endpointRole = $EndpointRole
    runId = if ($RunId) { $RunId } else { $null }
    collectionSemantics = 'Read-only metadata; no screen pixels, screenshots, input hooks, display changes, or private remote-provider API'
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
    processors = $processor
    graphicsAdapters = $graphicsAdapters
    monitorCatalog = $monitorCatalog
    displayEdid = $displayEdid
    pixelBridgeDecoder = [ordered]@{
        path = $decoderPath
        size = [UInt64]$decoderItem.Length
        sha256 = (Get-FileHash -LiteralPath $decoderPath -Algorithm SHA256).Hash.ToLowerInvariant()
        versionOutput = (& $decoderPath --version)
    }
    packageManifest = $packageSeal
}
Write-NewUtf8File -Path $resolvedOutput -Content ($fingerprint | ConvertTo-Json -Depth 20)
$fingerprint | ConvertTo-Json -Depth 20

#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MatrixSpecPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedMatrixSpecSha256,

    [Parameter(Mandatory = $true)]
    [string]$ComputerAMonitorCatalogPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedComputerAMonitorCatalogSha256,

    [Parameter(Mandatory = $true)]
    [string]$ExperimentMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

$matrixSpec = Import-PBRemoteVisualStep21MatrixSpec -Path $MatrixSpecPath -ExpectedSha256 $ExpectedMatrixSpecSha256
$monitorCatalogPath = [System.IO.Path]::GetFullPath($ComputerAMonitorCatalogPath)
$monitorCatalogIdentity = Get-PBFileIdentity -Path $monitorCatalogPath
if ([string]$monitorCatalogIdentity.sha256 -cne $ExpectedComputerAMonitorCatalogSha256 -or
    [UInt64]$monitorCatalogIdentity.size -gt 2MB)
{
    throw 'Computer A monitor catalog size or expected SHA-256 is invalid'
}
$monitorCatalog = Read-PBBoundedJson -Path $monitorCatalogPath -MaximumBytes 2MB
$monitorCount = Get-PBNonNegativeUInt64 -Value $monitorCatalog.monitorCount -Name 'Computer A monitor catalog monitorCount'
if ([string]$monitorCatalog.schema -cne 'PixelBridge.MonitorCatalog.1' -or $monitorCount -ne 2 -or
    @($monitorCatalog.monitors).Count -ne 2)
{
    throw 'Hardware-scoped Step 21 requires the authorized two-monitor Computer A catalog'
}
$experimentMatches = @($monitorCatalog.monitors | Where-Object { [string]$_.deviceName -ieq $ExperimentMonitorDeviceName })
if ($experimentMatches.Count -ne 1)
{
    throw 'ExperimentMonitorDeviceName is not unique in the Computer A monitor catalog'
}
$experimentMonitor = $experimentMatches[0]
$experimentDimensions = Get-PBRectDimensions -Rect $experimentMonitor.physicalRect -Name 'Computer A ExperimentMonitor physicalRect'
$experimentResolutionWidth = Get-PBNonNegativeUInt64 -Value $experimentMonitor.resolution.width `
    -Name 'Computer A ExperimentMonitor resolution width'
$experimentResolutionHeight = Get-PBNonNegativeUInt64 -Value $experimentMonitor.resolution.height `
    -Name 'Computer A ExperimentMonitor resolution height'
if ([string]$experimentMonitor.rotation -cne 'Identity' -or [UInt32]$experimentDimensions.width -ne 2560 -or
    [UInt32]$experimentDimensions.height -ne 1440 -or $experimentResolutionWidth -ne 2560 -or
    $experimentResolutionHeight -ne 1440)
{
    throw 'Authorized Step 21 hardware scope requires one unrotated 2560x1440 Computer A ExperimentMonitor'
}
$partition = New-PBRemoteVisualStep21HardwareScopePartition -Cells @($matrixSpec.value.cells) `
    -ExperimentMonitorWidth ([UInt32]$experimentDimensions.width) -ExperimentMonitorHeight ([UInt32]$experimentDimensions.height)
if (@($partition.includedCells).Count -ne 31 -or @($partition.excludedCells).Count -ne 10)
{
    throw 'Computer A monitor catalog did not produce the authorized 31 included and 10 excluded Step 21 cells'
}

$randomBytes = [byte[]]::new(16)
$random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try
{
    $random.GetBytes($randomBytes)
}
finally
{
    $random.Dispose()
}
$scopeId = ([BitConverter]::ToString($randomBytes)).Replace('-', '').ToLowerInvariant()
[Array]::Clear($randomBytes, 0, $randomBytes.Length)
if ($scopeId -cnotmatch '^[0-9a-f]{32}$')
{
    throw 'OS CSPRNG did not produce a valid Step 21 hardware-scope identity'
}

$scope = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21HardwareScope.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    scopeId = $scopeId
    scopeClass = 'Dual2560x1440SingleExperimentMonitor'
    matrixSpecification = $matrixSpec.identity
    computerAMonitorCatalog = $monitorCatalogIdentity
    experimentMonitor = [ordered]@{
        deviceName = [string]$experimentMonitor.deviceName
        physicalRect = $experimentMonitor.physicalRect
        resolution = $experimentMonitor.resolution
        rotation = [string]$experimentMonitor.rotation
    }
    fullCellCount = [UInt32]$matrixSpec.value.cellCount
    includedCellCount = [UInt32]@($partition.includedCells).Count
    includedCells = @($partition.includedCells)
    excludedCellCount = [UInt32]@($partition.excludedCells).Count
    excludedCells = @($partition.excludedCells)
    contracts = [ordered]@{
        sourceMatrixRemainsCanonical = $true
        cellsSelectedOnlyByRoiContainment = $true
        oneIndependentRunPerIncludedCell = $true
        excludedCellsDoNotCountAsCoverage = $true
        noPostRunScopeMutation = $true
        certifiedRemoteVisualProfile = $false
    }
}
$resolvedOutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$temporaryValidationPath = "$resolvedOutputPath.validation-$([Guid]::NewGuid().ToString('N')).json"
try
{
    $temporaryIdentity = Write-PBCreateOnlyJson -Path $temporaryValidationPath -Value $scope -Depth 40
    [void](Import-PBRemoteVisualStep21HardwareScope -Path $temporaryIdentity.path -ExpectedSha256 $temporaryIdentity.sha256)
}
finally
{
    if (Test-Path -LiteralPath $temporaryValidationPath -PathType Leaf)
    {
        [System.IO.File]::Delete($temporaryValidationPath)
    }
}
$identity = Write-PBCreateOnlyJson -Path $resolvedOutputPath -Value $scope -Depth 40
$validated = Import-PBRemoteVisualStep21HardwareScope -Path $identity.path -ExpectedSha256 $identity.sha256
[ordered]@{
    path = $validated.path
    size = [UInt64]$validated.identity.size
    sha256 = [string]$validated.identity.sha256
    scopeId = $scopeId
    scopeClass = [string]$scope.scopeClass
    matrixId = [string]$matrixSpec.value.matrixId
    fullCellCount = [UInt32]$scope.fullCellCount
    includedCellCount = [UInt32]$scope.includedCellCount
    excludedCellCount = [UInt32]$scope.excludedCellCount
    experimentMonitorDeviceName = [string]$experimentMonitor.deviceName
} | ConvertTo-Json -Depth 10

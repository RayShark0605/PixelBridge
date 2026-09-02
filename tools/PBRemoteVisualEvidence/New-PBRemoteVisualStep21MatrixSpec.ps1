#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('QualityPriority', 'Automatic', 'Restricted')]
    [string]$ComparisonModeClass,

    [Parameter(Mandatory = $true)]
    [ValidateSet(1, 2, 5)]
    [UInt32]$ComparisonLogicalFps,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

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
$matrixId = ([BitConverter]::ToString($randomBytes)).Replace('-', '').ToLowerInvariant()
[Array]::Clear($randomBytes, 0, $randomBytes.Length)
if ($matrixId -cnotmatch '^[0-9a-f]{32}$')
{
    throw 'OS CSPRNG did not produce a valid Step 21 matrix identity'
}
$cells = @(New-PBRemoteVisualStep21ExpectedCells -ComparisonModeClass $ComparisonModeClass `
    -ComparisonLogicalFps $ComparisonLogicalFps)
if ($cells.Count -ne 41)
{
    throw 'Internal Step 21 matrix specification did not produce exactly 41 cells'
}
$spec = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21MatrixSpec.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    matrixId = $matrixId
    comparisonModeClass = $ComparisonModeClass
    comparisonLogicalFps = $ComparisonLogicalFps
    cellCount = $cells.Count
    cells = $cells
    contracts = [ordered]@{
        oneIndependentRunPerCell = $true
        failedRunsAreRetained = $true
        differentRunMetricsMustNotBeMerged = $true
        uiVisibleModeRequired = $true
        unknownChromaAndLatencyRemainUnknown = $true
        invalidGeometryNeverResampled = $true
        certifiedRemoteVisualProfile = $false
    }
}
$identity = Write-PBCreateOnlyJson -Path $OutputPath -Value $spec -Depth 30
$validated = Import-PBRemoteVisualStep21MatrixSpec -Path $identity.path -ExpectedSha256 $identity.sha256
[ordered]@{
    path = $validated.path
    size = [UInt64]$validated.identity.size
    sha256 = [string]$validated.identity.sha256
    matrixId = $matrixId
    cellCount = $cells.Count
    comparisonModeClass = $ComparisonModeClass
    comparisonLogicalFps = $ComparisonLogicalFps
} | ConvertTo-Json -Depth 10

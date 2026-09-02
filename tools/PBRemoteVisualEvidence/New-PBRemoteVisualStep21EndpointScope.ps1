#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RunLedgerPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedRunLedgerSha256,

    [Parameter(Mandatory = $true)]
    [string]$RunLedgerSealPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedRunLedgerSealSha256,

    [Parameter(Mandatory = $true)]
    [string]$ComputerBMonitorCatalogPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedComputerBMonitorCatalogSha256,

    [Parameter(Mandatory = $true)]
    [string]$ComputerBProtectedMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$ComputerBExperimentMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

$ledger = Import-PBRemoteVisualStep21RunLedger -Path $RunLedgerPath -ExpectedSha256 $ExpectedRunLedgerSha256
$ledgerSeal = Import-PBRemoteVisualStep21RunLedgerSeal -Path $RunLedgerSealPath `
    -ExpectedSha256 $ExpectedRunLedgerSealSha256
Assert-PBMatrixIdentityEqual -Actual $ledgerSeal.ledger.identity -Expected $ledger.identity `
    -Name 'Step 21 endpoint-scope RunLedger and seal'
$computerB = Import-PBRemoteVisualStep21ComputerBMonitorCatalog -Path $ComputerBMonitorCatalogPath `
    -ExpectedSha256 $ExpectedComputerBMonitorCatalogSha256
$topology = New-PBRemoteVisualStep21EndpointTopologyValue -RunLedger $ledger -ComputerBCatalog $computerB.value `
    -ComputerBProtectedMonitorDeviceName $ComputerBProtectedMonitorDeviceName `
    -ComputerBExperimentMonitorDeviceName $ComputerBExperimentMonitorDeviceName

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container) -or
    (Test-Path -LiteralPath $resolvedOutput))
{
    throw 'Step 21 endpoint-scope output must be a new directory below an existing parent'
}
$outputParentItem = Get-Item -LiteralPath $outputParent
if (($outputParentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
{
    throw 'Step 21 endpoint-scope output parent must not be a reparse point'
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
$endpointScopeId = ([BitConverter]::ToString($randomBytes)).Replace('-', '').ToLowerInvariant()
[Array]::Clear($randomBytes, 0, $randomBytes.Length)
if ($endpointScopeId -cnotmatch '^[0-9a-f]{32}$')
{
    throw 'OS CSPRNG did not produce a valid Step 21 endpoint-scope identity'
}

$statement = 'This scope binds readiness catalogs and monitor roles only; it is not a live endpoint preflight, a matrix cell, file recovery evidence, or certification.'
$scope = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21EndpointScope.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'READINESS_ONLY'
    endpointScopeId = $endpointScopeId
    runLedger = $ledger.identity
    runLedgerSeal = $ledgerSeal.identity
    matrixSpecification = $ledger.matrixSpec.identity
    hardwareScope = $ledger.hardwareScope.identity
    computerAMonitorCatalog = $ledger.value.computerAMonitorCatalog
    computerBMonitorCatalog = $computerB.identity
    topology = $topology
    contracts = [ordered]@{
        runLedgerAndParentsImmutable = $true
        twoNonOverlappingExtendedMonitorsPerEndpoint = $true
        dataPixelsConfinedToExperimentMonitors = $true
        freshLiveMonitorCatalogRequiredPerRun = $true
        perRunDeploymentAndUiEvidenceRequired = $true
        noFileClipboardOrIpcPayloadSideChannel = $true
    }
    truthBoundary = [ordered]@{
        topologyReadinessOnly = $true
        formalStep21Accepted = $false
        executedCellCount = [UInt32]0
        runIdsAllocated = [UInt32]0
        providerUiEvidenceStillRequiredPerRun = $true
        liveCatalogRevalidationStillRequiredPerRun = $true
        certifiedRemoteVisualProfile = $false
        statement = $statement
    }
}

[void](New-Item -ItemType Directory -Path $resolvedOutput)
$scopePath = Join-Path $resolvedOutput 'step21-endpoint-scope.json'
$scopeIdentity = Write-PBCreateOnlyJson -Path $scopePath -Value $scope -Depth 40
[void](Import-PBRemoteVisualStep21EndpointScope -Path $scopeIdentity.path -ExpectedSha256 $scopeIdentity.sha256)

$sealArtifacts = @($ledger.identity, $ledgerSeal.identity, $ledger.matrixSpec.identity, $ledger.hardwareScope.identity,
    $ledger.value.computerAMonitorCatalog, $computerB.identity, $scopeIdentity)
$seal = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21EndpointScopeSeal.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'READINESS_ONLY'
    endpointScopeId = $endpointScopeId
    ledgerId = [string]$ledger.value.ledgerId
    matrixId = [string]$ledger.matrixSpec.value.matrixId
    scopeId = [string]$ledger.hardwareScope.value.scopeId
    artifactCount = [UInt32]$sealArtifacts.Count
    artifacts = $sealArtifacts
    executedCellCount = [UInt32]0
    formalStep21Accepted = $false
    certifiedRemoteVisualProfile = $false
}
$sealPath = Join-Path $resolvedOutput 'step21-endpoint-scope.seal.json'
$sealIdentity = Write-PBCreateOnlyJson -Path $sealPath -Value $seal -Depth 30
[void](Import-PBRemoteVisualStep21EndpointScopeSeal -Path $sealIdentity.path -ExpectedSha256 $sealIdentity.sha256)

[ordered]@{
    path = $resolvedOutput
    status = 'READINESS_ONLY'
    endpointScopeId = $endpointScopeId
    ledgerId = [string]$ledger.value.ledgerId
    matrixId = [string]$ledger.matrixSpec.value.matrixId
    scopeId = [string]$ledger.hardwareScope.value.scopeId
    computerAProtectedMonitor = [string]$topology.computerA.protectedMonitor.deviceName
    computerAExperimentMonitor = [string]$topology.computerA.experimentMonitor.deviceName
    computerBProtectedMonitor = [string]$topology.computerB.protectedMonitor.deviceName
    computerBExperimentMonitor = [string]$topology.computerB.experimentMonitor.deviceName
    encoderOriginX = [Int64]$topology.computerB.encoderOrigin.x
    encoderOriginY = [Int64]$topology.computerB.encoderOrigin.y
    scopeSha256 = [string]$scopeIdentity.sha256
    sealSha256 = [string]$sealIdentity.sha256
} | ConvertTo-Json -Depth 10

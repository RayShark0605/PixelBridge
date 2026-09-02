#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MatrixSpecPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedMatrixSpecSha256,

    [Parameter(Mandatory = $true)]
    [string]$HardwareScopePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedHardwareScopeSha256,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-CreateOnlyUtf8Text
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $resolvedPath)
    {
        throw "Create-only text output already exists: $resolvedPath"
    }
    $stream = [System.IO.File]::Open($resolvedPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
        try
        {
            $writer.Write($Text)
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

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

$matrixSpec = Import-PBRemoteVisualStep21MatrixSpec -Path $MatrixSpecPath -ExpectedSha256 $ExpectedMatrixSpecSha256
$hardwareScope = Import-PBRemoteVisualStep21HardwareScope -Path $HardwareScopePath `
    -ExpectedSha256 $ExpectedHardwareScopeSha256
Assert-PBMatrixIdentityEqual -Actual $hardwareScope.value.matrixSpecification -Expected $matrixSpec.identity `
    -Name 'Step 21 run-ledger parent MatrixSpec'

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container) -or
    (Test-Path -LiteralPath $resolvedOutput))
{
    throw 'Step 21 run-ledger output must be a new directory below an existing parent'
}
$outputParentItem = Get-Item -LiteralPath $outputParent
if (($outputParentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
{
    throw 'Step 21 run-ledger output parent must not be a reparse point'
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
$ledgerId = ([BitConverter]::ToString($randomBytes)).Replace('-', '').ToLowerInvariant()
[Array]::Clear($randomBytes, 0, $randomBytes.Length)
if ($ledgerId -cnotmatch '^[0-9a-f]{32}$')
{
    throw 'OS CSPRNG did not produce a valid Step 21 run-ledger identity'
}

$entries = [Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt [int]$hardwareScope.value.includedCellCount; $index++)
{
    [void]$entries.Add((New-PBRemoteVisualStep21RunLedgerEntry -Cell $hardwareScope.value.includedCells[$index] `
        -Ordinal ([UInt32]($index + 1)) -ExperimentMonitorRect $hardwareScope.value.experimentMonitor.physicalRect))
}
if ($entries.Count -ne 31)
{
    throw 'Step 21 run ledger requires exactly 31 hardware-scoped entries'
}

$statement = 'This ledger freezes an operator schedule only; no Step 21 matrix cell, provider behavior, file recovery, or certification is claimed.'
$ledger = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21RunLedger.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'NOT_EXECUTED'
    ledgerId = $ledgerId
    matrixSpecification = $matrixSpec.identity
    hardwareScope = $hardwareScope.identity
    computerAMonitorCatalog = $hardwareScope.value.computerAMonitorCatalog
    experimentMonitor = $hardwareScope.value.experimentMonitor
    fullCellCount = [UInt32]$hardwareScope.value.fullCellCount
    includedCellCount = [UInt32]$hardwareScope.value.includedCellCount
    entries = @($entries)
    excludedCellCount = [UInt32]$hardwareScope.value.excludedCellCount
    excludedCells = @($hardwareScope.value.excludedCells)
    contracts = [ordered]@{
        sourceMatrixAndScopeImmutable = $true
        oneIndependentRunPerIncludedCell = $true
        runIdAllocatedOnlyWhenCellExecutionBegins = $true
        perRunDeploymentUiPlanAndEvidenceRequired = $true
        captureRoiDoesNotEstablishScale = $true
        failedRunsMustBeClassifiedAndRetained = $true
    }
    truthBoundary = [ordered]@{
        formalStep21Accepted = $false
        executedCellCount = [UInt32]0
        pendingCellCount = [UInt32]$entries.Count
        full41CoverageCompleted = $false
        excludedCellsDoNotCountAsCoverage = $true
        acceptedLocatorGeometryAuthority = 'AcceptedBootstrapLocatorPixels'
        computerBMonitorCatalogStillRequired = $true
        providerUiEvidenceStillRequiredPerRun = $true
        certifiedRemoteVisualProfile = $false
        statement = $statement
    }
}

[void](New-Item -ItemType Directory -Path $resolvedOutput)
$ledgerPath = Join-Path $resolvedOutput 'step21-run-ledger.json'
$ledgerIdentity = Write-PBCreateOnlyJson -Path $ledgerPath -Value $ledger -Depth 40
[void](Import-PBRemoteVisualStep21RunLedger -Path $ledgerIdentity.path -ExpectedSha256 $ledgerIdentity.sha256)

$csvRows = foreach ($entry in $entries)
{
    [ordered]@{
        ordinal = [UInt32]$entry.ordinal
        cellId = [string]$entry.cellId
        profileToken = [string]$entry.profileToken
        captureBackend = [string]$entry.captureBackend
        modeClass = [string]$entry.modeClass
        scaleTarget = [string]$entry.scaleTarget
        logicalFps = [UInt32]$entry.logicalFps
        coverageRole = [string]$entry.coverageRole
        profileComparisonRole = [string]$entry.profileComparisonRole
        geometryMode = [string]$entry.geometryMode
        minimumTargetWidth = [UInt32]$entry.minimumTargetCanvas.width
        minimumTargetHeight = [UInt32]$entry.minimumTargetCanvas.height
        decoderCaptureRoiLeft = [Int64]$entry.decoderCaptureRoi.left
        decoderCaptureRoiTop = [Int64]$entry.decoderCaptureRoi.top
        decoderCaptureRoiRight = [Int64]$entry.decoderCaptureRoi.right
        decoderCaptureRoiBottom = [Int64]$entry.decoderCaptureRoi.bottom
        decoderCaptureRoiArgument = [string]$entry.decoderCaptureRoiArgument
        captureRoiPurpose = [string]$entry.captureRoiPurpose
        runDirectoryName = [string]$entry.runDirectoryName
        executionStatus = [string]$entry.executionStatus
        runId = ''
    }
}
$csvText = (@($csvRows) | ConvertTo-Csv -NoTypeInformation) -join "`r`n"
$csvText += "`r`n"
$csvPath = Join-Path $resolvedOutput 'step21-run-ledger.csv'
Write-CreateOnlyUtf8Text -Path $csvPath -Text $csvText
$csvIdentity = Get-PBFileIdentity -Path $csvPath
if (@(Import-Csv -LiteralPath $csvPath).Count -ne 31 -or $csvText.Contains('estimatedScaleX') -or
    $csvText.Contains('estimatedScaleY'))
{
    throw 'Step 21 run-ledger CSV did not preserve the exact 31-row schedule or contains legacy scale fields'
}

$sealArtifacts = @($matrixSpec.identity, $hardwareScope.identity, $hardwareScope.value.computerAMonitorCatalog,
    $ledgerIdentity, $csvIdentity)
$seal = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21RunLedgerSeal.1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    status = 'NOT_EXECUTED'
    ledgerId = $ledgerId
    matrixId = [string]$matrixSpec.value.matrixId
    scopeId = [string]$hardwareScope.value.scopeId
    includedCellCount = [UInt32]$entries.Count
    excludedCellCount = [UInt32]$hardwareScope.value.excludedCellCount
    artifactCount = [UInt32]$sealArtifacts.Count
    artifacts = $sealArtifacts
    formalStep21Accepted = $false
    certifiedRemoteVisualProfile = $false
}
$sealPath = Join-Path $resolvedOutput 'step21-run-ledger.seal.json'
$sealIdentity = Write-PBCreateOnlyJson -Path $sealPath -Value $seal -Depth 30
[void](Import-PBRemoteVisualStep21RunLedgerSeal -Path $sealIdentity.path -ExpectedSha256 $sealIdentity.sha256)

[ordered]@{
    path = $resolvedOutput
    status = 'NOT_EXECUTED'
    ledgerId = $ledgerId
    matrixId = [string]$matrixSpec.value.matrixId
    scopeId = [string]$hardwareScope.value.scopeId
    includedCellCount = [UInt32]$entries.Count
    excludedCellCount = [UInt32]$hardwareScope.value.excludedCellCount
    ledgerSha256 = [string]$ledgerIdentity.sha256
    csvSha256 = [string]$csvIdentity.sha256
    sealSha256 = [string]$sealIdentity.sha256
} | ConvertTo-Json -Depth 10

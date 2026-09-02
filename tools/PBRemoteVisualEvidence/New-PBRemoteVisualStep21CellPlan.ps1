#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EndpointScopePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedEndpointScopeSha256,

    [Parameter(Mandatory = $true)]
    [string]$EndpointScopeSealPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedEndpointScopeSealSha256,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 31)]
    [UInt32]$CellOrdinal,

    [Parameter(Mandatory = $true)]
    [string]$DeploymentManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$UiEvidencePath,

    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [ValidateRange(-300000, 300000)]
    [int]$DecoderClockOffsetMilliseconds = 0,

    [ValidateRange(0, 300000)]
    [int]$ClockUncertaintyMilliseconds = 5000,

    [ValidateLength(0, 4096)]
    [string]$Notes = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
$matrixModule = Join-Path $PSScriptRoot 'PBRemoteVisualMatrixCommon.psm1'
Import-Module -Name $pilotModule -Force -ErrorAction Stop
Import-Module -Name $matrixModule -Force -ErrorAction Stop

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$outputParent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputParent) -or -not (Test-Path -LiteralPath $outputParent -PathType Container) -or
    ((Get-Item -LiteralPath $outputParent -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
{
    throw 'Step 21 cell-plan output parent must be an existing regular directory'
}
if (Test-Path -LiteralPath $resolvedOutput)
{
    throw "Step 21 cell-plan create-only output already exists: $resolvedOutput"
}

$endpointScope = Import-PBRemoteVisualStep21EndpointScope -Path $EndpointScopePath `
    -ExpectedSha256 $ExpectedEndpointScopeSha256
$endpointScopeSeal = Import-PBRemoteVisualStep21EndpointScopeSeal -Path $EndpointScopeSealPath `
    -ExpectedSha256 $ExpectedEndpointScopeSealSha256
Assert-PBMatrixIdentityEqual -Actual $endpointScope.identity -Expected $endpointScopeSeal.scope.identity `
    -Name 'Step 21 cell-plan EndpointScope and seal'

$cellArguments = Get-PBRemoteVisualStep21CellPlanArguments -EndpointScope $endpointScope -CellOrdinal $CellOrdinal
$bindingNote = "Step21CellOrdinal=$($cellArguments.cellOrdinal);CellId=$($cellArguments.cellId);EndpointScopeSha256=$($endpointScope.identity.sha256)"
$effectiveNotes = if ([string]::IsNullOrWhiteSpace($Notes)) { $bindingNote } else { "$bindingNote; $Notes" }
$temporaryPath = Join-Path $outputParent ("." + [System.IO.Path]::GetFileName($resolvedOutput) +
    ".cell-validation-" + [Guid]::NewGuid().ToString('N') + '.json')
$planCreator = Join-Path $PSScriptRoot 'New-PBRemoteVisualPilotPlan.ps1'
$movedToFinal = $false
$published = $false
try
{
    $creatorParameters = @{
        DeploymentManifestPath = [System.IO.Path]::GetFullPath($DeploymentManifestPath)
        UiEvidencePath = [System.IO.Path]::GetFullPath($UiEvidencePath)
        PythonPath = [System.IO.Path]::GetFullPath($PythonPath)
        OutputPath = $temporaryPath
        ProfileToken = [string]$cellArguments.profileToken
        CaptureBackend = [string]$cellArguments.captureBackend
        MatrixModeClass = [string]$cellArguments.matrixModeClass
        MatrixScaleTarget = [string]$cellArguments.matrixScaleTarget
        LogicalFps = [int]$cellArguments.logicalFps
        GeometryMode = [string]$cellArguments.geometryMode
        EncoderProtectedMonitorDeviceName = [string]$cellArguments.encoderProtectedMonitorDeviceName
        EncoderExperimentMonitorDeviceName = [string]$cellArguments.encoderExperimentMonitorDeviceName
        EncoderOriginX = [int]$cellArguments.encoderOriginX
        EncoderOriginY = [int]$cellArguments.encoderOriginY
        DecoderProtectedMonitorDeviceName = [string]$cellArguments.decoderProtectedMonitorDeviceName
        DecoderExperimentMonitorDeviceName = [string]$cellArguments.decoderExperimentMonitorDeviceName
        DecoderRoiLeft = [int]$cellArguments.decoderRoiLeft
        DecoderRoiTop = [int]$cellArguments.decoderRoiTop
        DecoderRoiRight = [int]$cellArguments.decoderRoiRight
        DecoderRoiBottom = [int]$cellArguments.decoderRoiBottom
        DecoderClockOffsetMilliseconds = $DecoderClockOffsetMilliseconds
        ClockUncertaintyMilliseconds = $ClockUncertaintyMilliseconds
        Notes = $effectiveNotes
    }
    $creatorOutputText = @(& $planCreator @creatorParameters 2>&1) -join "`n"
    $creatorOutput = ConvertFrom-PBStrictJsonText -Text $creatorOutputText -Name 'Step 21 cell-plan creator output'
    $temporaryPlan = Import-PBRemoteVisualPilotPlan -Path $temporaryPath -ExpectedSha256 ([string]$creatorOutput.sha256)
    $binding = Assert-PBRemoteVisualStep21CellPlanBinding -EndpointScope $endpointScope `
        -CellOrdinal $CellOrdinal -Plan $temporaryPlan.value
    if ([string]$creatorOutput.path -cne $temporaryPlan.path -or [string]$creatorOutput.runId -cne [string]$binding.runId)
    {
        throw 'Step 21 cell-plan creator receipt differs from the validated temporary plan'
    }
    [System.IO.File]::Move($temporaryPath, $resolvedOutput)
    $movedToFinal = $true
    $finalPlan = Import-PBRemoteVisualPilotPlan -Path $resolvedOutput -ExpectedSha256 $temporaryPlan.identity.sha256
    $finalBinding = Assert-PBRemoteVisualStep21CellPlanBinding -EndpointScope $endpointScope `
        -CellOrdinal $CellOrdinal -Plan $finalPlan.value
    $published = $true
    [ordered]@{
        schema = 'PixelBridge.RemoteVisualStep21CellPlanCreation.1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        status = 'PLAN_READY_NOT_EXECUTED'
        cellOrdinal = [UInt32]$finalBinding.cellOrdinal
        cellId = [string]$finalBinding.cellId
        runDirectoryName = [string]$finalBinding.runDirectoryName
        runId = [string]$finalBinding.runId
        endpointScope = $endpointScope.identity
        endpointScopeSeal = $endpointScopeSeal.identity
        plan = $finalPlan.identity
        tuple = [ordered]@{
            profileToken = [string]$finalBinding.profileToken
            captureBackend = [string]$finalBinding.captureBackend
            matrixModeClass = [string]$finalBinding.matrixModeClass
            matrixScaleTarget = [string]$finalBinding.matrixScaleTarget
            logicalFps = [UInt32]$finalBinding.logicalFps
            geometryMode = [string]$finalBinding.geometryMode
        }
        truthBoundary = [ordered]@{
            planCreationIsNotCellExecution = $true
            liveEndpointPreflightStillRequired = $true
            decoderMustStartBeforeEncoder = $true
            fieldEvidenceStillRequired = $true
            formalStep21Accepted = $false
            certifiedRemoteVisualProfile = $false
        }
    } | ConvertTo-Json -Depth 12
}
finally
{
    if (-not $published)
    {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
        {
            [System.IO.File]::Delete($temporaryPath)
        }
        if ($movedToFinal -and (Test-Path -LiteralPath $resolvedOutput -PathType Leaf))
        {
            [System.IO.File]::Delete($resolvedOutput)
        }
    }
}

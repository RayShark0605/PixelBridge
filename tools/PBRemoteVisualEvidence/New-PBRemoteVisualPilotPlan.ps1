#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DeploymentManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$UiEvidencePath,

    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet(1, 2, 5)]
    [int]$LogicalFps,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Strict1To1', 'LocatorScaled')]
    [string]$GeometryMode,

    [ValidateSet('direct', 'shape', 'remote-lf4')]
    [string]$ProfileToken = 'remote-lf4',

    [ValidateSet('wgc', 'dxgi')]
    [string]$CaptureBackend = 'wgc',

    [string]$MatrixModeClass = '',

    [Parameter(Mandatory = $true)]
    [string]$EncoderProtectedMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$EncoderExperimentMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [int]$EncoderOriginX,

    [Parameter(Mandatory = $true)]
    [int]$EncoderOriginY,

    [Parameter(Mandatory = $true)]
    [string]$DecoderProtectedMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$DecoderExperimentMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [int]$DecoderRoiLeft,

    [Parameter(Mandatory = $true)]
    [int]$DecoderRoiTop,

    [Parameter(Mandatory = $true)]
    [int]$DecoderRoiRight,

    [Parameter(Mandatory = $true)]
    [int]$DecoderRoiBottom,

    [ValidateRange(-300000, 300000)]
    [int]$DecoderClockOffsetMilliseconds = 0,

    [ValidateRange(0, 300000)]
    [int]$ClockUncertaintyMilliseconds = 5000,

    [string]$Notes = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop

function Get-EndpointEnvironment
{
    param(
        [Parameter(Mandatory = $true)][object]$Deployment,
        [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'Decoder')][string]$Role
    )
    $matches = @($Deployment.artifacts.endpointEnvironments | Where-Object { [string]$_.role -ceq $Role })
    if ($matches.Count -ne 1)
    {
        throw "Deployment must contain exactly one $Role environment artifact"
    }
    $identity = $matches[0].artifact
    $path = [System.IO.Path]::GetFullPath([string]$identity.path)
    [void](Assert-PBFileIdentity -Path $path -Expected $identity -Name "$Role environment")
    $environment = Read-PBBoundedJson -Path $path -MaximumBytes 2MB
    if ([string]$environment.schema -cne 'PixelBridge.EndpointEnvironment.2' -or
        [string]$environment.endpointRole -cne $Role -or [string]$environment.runId -cne [string]$Deployment.runId)
    {
        throw "$Role environment schema, role, or RunId mismatch"
    }
    return [ordered]@{ path = $path; identity = Get-PBFileIdentity -Path $path; value = $environment }
}

function Get-ExactMonitor
{
    param(
        [Parameter(Mandatory = $true)][object]$Environment,
        [Parameter(Mandatory = $true)][string]$DeviceName,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if ([string]::IsNullOrWhiteSpace($DeviceName))
    {
        throw "$Description device name must be explicit"
    }
    $matches = @($Environment.monitorCatalog.monitors | Where-Object { [string]$_.deviceName -ieq $DeviceName })
    if ($matches.Count -ne 1)
    {
        throw "$Description monitor is not unique in the endpoint environment"
    }
    if ([string]$matches[0].rotation -cne 'Identity')
    {
        throw "$Description monitor rotation is unsupported by the Step 20 axis-aligned pilot"
    }
    [void](Get-PBRectDimensions -Rect $matches[0].physicalRect -Name "$Description physicalRect")
    return $matches[0]
}

function Get-ApplicationIdentity
{
    param(
        [Parameter(Mandatory = $true)][object]$PackageManifest,
        [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'Decoder')][string]$Role
    )
    $applicationMatches = @($PackageManifest.applications | Where-Object { [string]$_.role -ceq $Role })
    if ($applicationMatches.Count -ne 1 -or [string]$applicationMatches[0].relativeExecutablePath -cnotmatch "^$Role/PixelBridge$Role\.exe$" -or
        [UInt64]$applicationMatches[0].size -eq 0 -or [string]$applicationMatches[0].sha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "Package manifest does not contain one valid $Role application identity"
    }
    return [ordered]@{
        role = $Role
        relativeExecutablePath = [string]$applicationMatches[0].relativeExecutablePath
        size = [UInt64]$applicationMatches[0].size
        sha256 = [string]$applicationMatches[0].sha256
        versionOutput = [string]$applicationMatches[0].versionOutput
    }
}

function Get-MonitorRuntimeContract
{
    param([Parameter(Mandatory = $true)][object]$Monitor)
    return [ordered]@{
        deviceName = [string]$Monitor.deviceName
        physicalRect = $Monitor.physicalRect
        dpiX = [UInt32]$Monitor.dpiX
        dpiY = [UInt32]$Monitor.dpiY
        refreshRate = [UInt32]$Monitor.refreshRate
        rotation = [string]$Monitor.rotation
        adapterLuid = $Monitor.adapterLuid
        primary = [bool]$Monitor.primary
    }
}

$resolvedDeployment = [System.IO.Path]::GetFullPath($DeploymentManifestPath)
$resolvedUiEvidence = [System.IO.Path]::GetFullPath($UiEvidencePath)
$resolvedPython = [System.IO.Path]::GetFullPath($PythonPath)
if (-not (Test-Path -LiteralPath $resolvedPython -PathType Leaf))
{
    throw "Python interpreter does not exist: $resolvedPython"
}
$deploymentVerificationText = @(& (Join-Path $PSScriptRoot 'Test-PBRemoteVisualDeploymentManifest.ps1') `
    -DeploymentManifestPath $resolvedDeployment 2>&1)
$deploymentVerification = ConvertFrom-PBStrictJsonText -Text ($deploymentVerificationText -join "`n") -Name 'deployment verification output'
if ($deploymentVerification.verified -isnot [bool] -or -not [bool]$deploymentVerification.verified)
{
    throw 'Deployment verifier did not return a verified result'
}
$deployment = Read-PBBoundedJson -Path $resolvedDeployment -MaximumBytes 2MB
$uiEvidence = Read-PBBoundedJson -Path $resolvedUiEvidence -MaximumBytes 128KB
$uiMetadataPath = [System.IO.Path]::GetFullPath([string]$uiEvidence.metadata.path)
$uiScreenshotPath = [System.IO.Path]::GetFullPath([string]$uiEvidence.screenshot.path)
$uiCaptureRecordPath = [System.IO.Path]::GetFullPath([string]$uiEvidence.captureRecord.path)
$uiVerificationText = @(& (Join-Path $PSScriptRoot 'Test-PBRemoteVisualUiEvidence.ps1') `
    -UiEvidencePath $resolvedUiEvidence -MetadataPath $uiMetadataPath -ScreenshotPath $uiScreenshotPath `
    -CaptureRecordPath $uiCaptureRecordPath -PythonPath $resolvedPython 2>&1)
$uiVerification = ConvertFrom-PBStrictJsonText -Text ($uiVerificationText -join "`n") -Name 'remote UI verification output'
if ([string]$deployment.runId -cne [string]$uiVerification.runId -or
    [string]$deployment.runId -cne [string]$uiEvidence.runId -or
    [string]$deployment.artifacts.remoteMetadata.sha256 -cne [string]$uiEvidence.metadata.sha256)
{
    throw 'Deployment and remote UI evidence do not share the same RunId/metadata identity'
}

$isStep20Plan = $ProfileToken -ceq 'remote-lf4' -and $CaptureBackend -ceq 'wgc' -and
    [string]::IsNullOrEmpty($MatrixModeClass)
if (-not $isStep20Plan -and $MatrixModeClass -notin @('QualityPriority', 'Automatic', 'Restricted'))
{
    throw 'Step 21 field-run plans require MatrixModeClass QualityPriority, Automatic, or Restricted'
}
$profileContract = switch ($ProfileToken)
{
    'direct'
    {
        [ordered]@{
            name = 'Direct-Level 2x2 (Experimental)'
            visualProfileId = [UInt64]::Parse('EBB15DCE41AB436E', [Globalization.NumberStyles]::HexNumber)
            visualLayoutVersion = 3
            codedDataBytesPerFrame = 86688
            codewordsPerFrame = 42
            scaledGeometry = $false
        }
    }
    'shape'
    {
        [ordered]@{
            name = 'Shape+Chroma (Experimental)'
            visualProfileId = [UInt64]::Parse('5042534843503031', [Globalization.NumberStyles]::HexNumber)
            visualLayoutVersion = 4
            codedDataBytesPerFrame = 65016
            codewordsPerFrame = 32
            scaledGeometry = $false
        }
    }
    'remote-lf4'
    {
        [ordered]@{
            name = 'PB-RemoteVisual-LF4-X1 (Experimental)'
            visualProfileId = [UInt64]::Parse('504252564C463431', [Globalization.NumberStyles]::HexNumber)
            visualLayoutVersion = 7
            codedDataBytesPerFrame = 8100
            codewordsPerFrame = 4
            scaledGeometry = $true
        }
    }
}

$encoderEnvironment = Get-EndpointEnvironment -Deployment $deployment -Role Encoder
$decoderEnvironment = Get-EndpointEnvironment -Deployment $deployment -Role Decoder
$encoderProtectedMonitor = Get-ExactMonitor -Environment $encoderEnvironment.value -DeviceName $EncoderProtectedMonitorDeviceName -Description 'Encoder ProtectedMonitor'
$encoderExperimentMonitor = Get-ExactMonitor -Environment $encoderEnvironment.value -DeviceName $EncoderExperimentMonitorDeviceName -Description 'Encoder ExperimentMonitor'
$decoderProtectedMonitor = Get-ExactMonitor -Environment $decoderEnvironment.value -DeviceName $DecoderProtectedMonitorDeviceName -Description 'Decoder ProtectedMonitor'
$decoderExperimentMonitor = Get-ExactMonitor -Environment $decoderEnvironment.value -DeviceName $DecoderExperimentMonitorDeviceName -Description 'Decoder ExperimentMonitor'
if ([string]$encoderProtectedMonitor.deviceName -ieq [string]$encoderExperimentMonitor.deviceName -or
    [string]$decoderProtectedMonitor.deviceName -ieq [string]$decoderExperimentMonitor.deviceName)
{
    throw 'ProtectedMonitor and ExperimentMonitor must be distinct at both endpoints'
}
if (Test-PBRectsOverlap -First $encoderProtectedMonitor.physicalRect -Second $encoderExperimentMonitor.physicalRect)
{
    throw 'Encoder ProtectedMonitor and ExperimentMonitor overlap'
}
if (Test-PBRectsOverlap -First $decoderProtectedMonitor.physicalRect -Second $decoderExperimentMonitor.physicalRect)
{
    throw 'Decoder ProtectedMonitor and ExperimentMonitor overlap'
}

$dataWindow = [ordered]@{
    left = $EncoderOriginX
    top = $EncoderOriginY
    right = [Int64]$EncoderOriginX + 1920
    bottom = [Int64]$EncoderOriginY + 1080
}
$decoderRoi = [ordered]@{ left = $DecoderRoiLeft; top = $DecoderRoiTop; right = $DecoderRoiRight; bottom = $DecoderRoiBottom }
$dataWindowDimensions = Get-PBRectDimensions -Rect $dataWindow -Name 'Encoder Data Window'
$decoderRoiDimensions = Get-PBRectDimensions -Rect $decoderRoi -Name 'Decoder ROI'
if (-not (Test-PBRectContains -Outer $encoderExperimentMonitor.physicalRect -Inner $dataWindow) -or
    (Test-PBRectsOverlap -First $encoderProtectedMonitor.physicalRect -Second $dataWindow))
{
    throw 'Encoder Data Window is not fully contained by the Encoder ExperimentMonitor'
}
if (-not (Test-PBRectContains -Outer $decoderExperimentMonitor.physicalRect -Inner $decoderRoi) -or
    (Test-PBRectsOverlap -First $decoderProtectedMonitor.physicalRect -Second $decoderRoi))
{
    throw 'Decoder ROI is not fully contained by the Decoder ExperimentMonitor'
}
if ([string]$uiEvidence.captureRecord.experimentMonitorDeviceName -cne [string]$decoderExperimentMonitor.deviceName -or
    [string]$uiEvidence.captureRecord.protectedMonitorDeviceName -cne [string]$decoderProtectedMonitor.deviceName)
{
    throw 'Remote UI screenshot was not captured from the Decoder endpoint monitor pair selected by the plan'
}
$scaleX = [double]$decoderRoiDimensions.width / 1920.0
$scaleY = [double]$decoderRoiDimensions.height / 1080.0
if ([bool]$profileContract.scaledGeometry -and
    ($scaleX -lt 0.5 -or $scaleX -gt 2.0 -or $scaleY -lt 0.5 -or $scaleY -gt 2.0))
{
    throw 'Decoder ROI is outside the LF4 0.5..2.0 continuous scale domain'
}
if (-not [bool]$profileContract.scaledGeometry -and
    ($decoderRoiDimensions.width -ne 1920 -or $decoderRoiDimensions.height -ne 1080 -or $GeometryMode -cne 'Strict1To1'))
{
    throw 'Direct and Shape field-run plans require Strict1To1 with an exact 1920x1080 Decoder ROI; no silent resampling is permitted'
}
if ($GeometryMode -ceq 'Strict1To1' -and ($decoderRoiDimensions.width -ne 1920 -or $decoderRoiDimensions.height -ne 1080))
{
    throw 'Strict1To1 plan requires an exact 1920x1080 Decoder ROI'
}
if ($GeometryMode -ceq 'LocatorScaled' -and $decoderRoiDimensions.width -eq 1920 -and $decoderRoiDimensions.height -eq 1080)
{
    throw 'LocatorScaled plan must exercise a non-1:1 Decoder ROI'
}

$sourceManifestPath = [System.IO.Path]::GetFullPath([string]$deployment.artifacts.sourceManifest.path)
[void](Assert-PBFileIdentity -Path $sourceManifestPath -Expected $deployment.artifacts.sourceManifest -Name 'source manifest')
$sourceManifest = Read-PBBoundedJson -Path $sourceManifestPath -MaximumBytes 256KB
$sourceMatches = @($sourceManifest.files | Where-Object { [string]$_.path -ceq 'random-1MiB.bin' })
if ($sourceMatches.Count -ne 1 -or [UInt64]$sourceMatches[0].size -ne 1MB -or
    [string]$sourceMatches[0].sha256 -cnotmatch '^[0-9a-f]{64}$' -or
    [string]$sourceMatches[0].pixelBridgeSegmentCompression -cne 'RAW/OFF')
{
    throw 'Source manifest does not contain the frozen 1 MiB RAW pilot source'
}
$sourcePath = Join-Path ([System.IO.Path]::GetDirectoryName($sourceManifestPath)) 'random-1MiB.bin'
$sourceIdentity = Get-PBFileIdentity -Path $sourcePath
if ([UInt64]$sourceIdentity.size -ne [UInt64]$sourceMatches[0].size -or [string]$sourceIdentity.sha256 -cne [string]$sourceMatches[0].sha256)
{
    throw 'Frozen 1 MiB source file differs from its source-set manifest'
}

$packageManifestPath = [System.IO.Path]::GetFullPath([string]$deployment.artifacts.packageManifest.path)
[void](Assert-PBFileIdentity -Path $packageManifestPath -Expected $deployment.artifacts.packageManifest -Name 'package manifest')
$packageManifest = Read-PBBoundedJson -Path $packageManifestPath -MaximumBytes 4MB
$encoderApplication = Get-ApplicationIdentity -PackageManifest $packageManifest -Role Encoder
$decoderApplication = Get-ApplicationIdentity -PackageManifest $packageManifest -Role Decoder
$policyValues = switch ($LogicalFps)
{
    1 { [ordered]@{ encoderSeconds = 600; decoderSeconds = 540; noProgressSeconds = 120; replaySampleFps = 2 } }
    2 { [ordered]@{ encoderSeconds = 360; decoderSeconds = 300; noProgressSeconds = 90; replaySampleFps = 4 } }
    5 { [ordered]@{ encoderSeconds = 180; decoderSeconds = 120; noProgressSeconds = 60; replaySampleFps = 10 } }
}
if (-not $isStep20Plan -and [Math]::Max($scaleX, $scaleY) -gt 1.3)
{
    # The 16 GiB hard cap cannot hold a 2x time-sampled 1.5x ROI for the frozen run windows.
    # Sampling at the logical raster ceiling remains unthrottled from primary demod and is
    # independently accepted only when the resulting Replay reproduces the complete file.
    $policyValues.replaySampleFps = $LogicalFps
}
$worstCaseSampledFrames = [UInt64]$policyValues.replaySampleFps * ([UInt64]$policyValues.decoderSeconds + 2) + 2
$maximumReplayFrames = if ($isStep20Plan) { [UInt32]1250 } else { [UInt32]$worstCaseSampledFrames }
$worstCaseReplayBytes = [UInt64]$decoderRoiDimensions.width * [UInt64]$decoderRoiDimensions.height * 4 * $maximumReplayFrames + 256MB
if ($worstCaseSampledFrames -gt 1250 -or $maximumReplayFrames -eq 0 -or $worstCaseReplayBytes -gt 16GB)
{
    throw 'Selected Decoder ROI exceeds the frozen Replay frame or byte resource envelope'
}

$deploymentManifestIdentity = Get-PBFileIdentity -Path $resolvedDeployment
$plan = [ordered]@{
    schema = if ($isStep20Plan) { 'PixelBridge.RemoteVisualPilotPlan.1' } else { 'PixelBridge.RemoteVisualPilotPlan.2' }
    createdUtc = [DateTime]::UtcNow.ToString('o')
    runId = [string]$deployment.runId
    profileToken = $ProfileToken
    profileName = [string]$profileContract.name
    visualProfileId = [UInt64]$profileContract.visualProfileId
    visualLayoutVersion = [UInt32]$profileContract.visualLayoutVersion
    codedDataBytesPerFrame = [UInt32]$profileContract.codedDataBytesPerFrame
    codewordsPerFrame = [UInt32]$profileContract.codewordsPerFrame
    logicalFps = $LogicalFps
    geometryMode = $GeometryMode
    deployment = [ordered]@{
        runId = [string]$deployment.runId
        manifest = $deploymentManifestIdentity
        deploymentFingerprintSha256 = [string]$deployment.deploymentFingerprintSha256
        packageManifest = Get-PBFileIdentity -Path $packageManifestPath
        sourceManifest = Get-PBFileIdentity -Path $sourceManifestPath
        remoteMetadata = Get-PBFileIdentity -Path ([string]$deployment.artifacts.remoteMetadata.path)
        encoderEnvironment = $encoderEnvironment.identity
        decoderEnvironment = $decoderEnvironment.identity
    }
    applications = [ordered]@{ encoder = $encoderApplication; decoder = $decoderApplication }
    source = [ordered]@{
        sourceSetId = [string]$sourceManifest.sourceSetId
        relativePath = 'random-1MiB.bin'
        size = [UInt64]$sourceMatches[0].size
        sha256 = [string]$sourceMatches[0].sha256
        pixelBridgeSegmentCompression = 'RAW/OFF'
    }
    remoteUi = [ordered]@{
        runId = [string]$uiEvidence.runId
        provenance = 'RemoteUiVisible'
        evidence = Get-PBFileIdentity -Path $resolvedUiEvidence
        screenshot = Get-PBFileIdentity -Path $uiScreenshotPath
        captureRecord = Get-PBFileIdentity -Path $uiCaptureRecordPath
        visibleFields = @($uiEvidence.visibleFields)
        visibleClaims = $uiEvidence.visibleClaims
    }
    monitorSafety = [ordered]@{
        encoder = [ordered]@{
            protectedMonitorDeviceName = [string]$encoderProtectedMonitor.deviceName
            experimentMonitorDeviceName = [string]$encoderExperimentMonitor.deviceName
            protectedMonitorPhysicalRect = $encoderProtectedMonitor.physicalRect
            experimentMonitorPhysicalRect = $encoderExperimentMonitor.physicalRect
            dataWindowPhysicalRect = $dataWindow
        }
        decoder = [ordered]@{
            protectedMonitorDeviceName = [string]$decoderProtectedMonitor.deviceName
            experimentMonitorDeviceName = [string]$decoderExperimentMonitor.deviceName
            protectedMonitorPhysicalRect = $decoderProtectedMonitor.physicalRect
            experimentMonitorPhysicalRect = $decoderExperimentMonitor.physicalRect
            roiPhysicalRect = $decoderRoi
        }
    }
    remoteGeometry = [ordered]@{
        estimatedScaleX = $scaleX
        estimatedScaleY = $scaleY
        axisAligned = $true
        cropStatus = 'NoneExpected'
        locatorRemainsAuthoritative = $true
    }
    policy = [ordered]@{
        captureBackend = $CaptureBackend
        compression = 'off'
        encoderHardMaximumSeconds = $policyValues.encoderSeconds
        decoderTimeoutSeconds = $policyValues.decoderSeconds
        noProgressSeconds = $policyValues.noProgressSeconds
        offlineReplayTimeoutSeconds = 600
        offlineNoProgressSeconds = 120
        decoderWarmupMaximumSeconds = 30
        controlRepetitions = 12
        postPublishReplayTailSeconds = 2
        postDecoderBroadcastProofSeconds = [UInt32]([Math]::Ceiling($ClockUncertaintyMilliseconds / 1000.0) + 2)
        manualStopRequired = $true
        decoderStartsBeforeEncoder = $true
        noFileClipboardOrIpcSideChannel = $true
        replay = [ordered]@{
            maximumCaptureFramesPerSecond = $policyValues.replaySampleFps
            maximumFrames = $maximumReplayFrames
            maximumMiB = 16384
            reservedNonFrameMiB = 256
            worstCaseSampledFrames = $worstCaseSampledFrames
            worstCaseBytesIncludingReserve = $worstCaseReplayBytes
            liveAndOfflineRequired = $true
        }
        clock = [ordered]@{
            decoderOffsetMilliseconds = $DecoderClockOffsetMilliseconds
            uncertaintyMilliseconds = $ClockUncertaintyMilliseconds
        }
    }
    completionContract = [ordered]@{
        decoderCompletedWhileEncoderBroadcasting = $true
        wholeFileDigestRequired = $true
        safePublishRequired = $true
        externalLengthAndSha256Required = $true
        replayOfflineReproductionRequired = $true
        zeroFalseOutputRequired = $true
        failureClassification = @('geometry', 'signal', 'temporal', 'metric', 'scheduler')
    }
    notes = $Notes
}
if (-not $isStep20Plan)
{
    $plan['matrix'] = [ordered]@{
        modeClass = $MatrixModeClass
        visibleRemoteMode = [string]$uiEvidence.visibleClaims.remoteMode
        profileComparisonRole = if ($ProfileToken -ceq 'remote-lf4') { 'Candidate' } else { 'Baseline' }
        backendCoverageRole = if ($CaptureBackend -ceq 'wgc') { 'MainMatrix' } else { 'RepresentativeRecheck' }
        runIsolation = 'IndependentRunIdAndArtifacts; metrics from different runs must not be merged'
    }
    $plan.monitorSafety.encoder['runtimeEnforcement'] = if ($ProfileToken -ceq 'remote-lf4') {
        'ProductionRuntimePreflightAndPeriodicRevalidation'
    } else {
        'PlanAndEndpointEnvironmentContainment; current Direct/Shape Encoder CLI does not accept monitor-safety arguments'
    }
    $plan.monitorSafety.encoder['protectedMonitorContract'] = Get-MonitorRuntimeContract -Monitor $encoderProtectedMonitor
    $plan.monitorSafety.encoder['experimentMonitorContract'] = Get-MonitorRuntimeContract -Monitor $encoderExperimentMonitor
    $plan.monitorSafety.decoder['runtimeEnforcement'] = 'ProductionRuntimePreflightAndPeriodicRevalidation'
    $plan.monitorSafety.decoder['protectedMonitorContract'] = Get-MonitorRuntimeContract -Monitor $decoderProtectedMonitor
    $plan.monitorSafety.decoder['experimentMonitorContract'] = Get-MonitorRuntimeContract -Monitor $decoderExperimentMonitor
}

$temporaryValidationPath = "$([System.IO.Path]::GetFullPath($OutputPath)).validation-$([Guid]::NewGuid().ToString('N')).json"
try
{
    [void](Write-PBCreateOnlyJson -Path $temporaryValidationPath -Value $plan -Depth 30)
    [void](Import-PBRemoteVisualPilotPlan -Path $temporaryValidationPath)
}
finally
{
    if (Test-Path -LiteralPath $temporaryValidationPath -PathType Leaf)
    {
        [System.IO.File]::Delete($temporaryValidationPath)
    }
}
$identity = Write-PBCreateOnlyJson -Path $OutputPath -Value $plan -Depth 30
$validated = Import-PBRemoteVisualPilotPlan -Path ([System.IO.Path]::GetFullPath($OutputPath)) -ExpectedSha256 $identity.sha256
[ordered]@{
    path = $validated.path
    size = $validated.identity.size
    sha256 = $validated.identity.sha256
    runId = [string]$plan.runId
    logicalFps = $LogicalFps
    geometryMode = $GeometryMode
    decoderRoi = $decoderRoi
    replayWorstCaseSampledFrames = $worstCaseSampledFrames
    replayWorstCaseBytesIncludingReserve = $worstCaseReplayBytes
} | ConvertTo-Json -Depth 10

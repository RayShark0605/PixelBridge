#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolsRoot,

    [Parameter(Mandatory = $true)]
    [string]$WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-NewText
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $bytes = [System.Text.UTF8Encoding]::new($false, $true).GetBytes($Text)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally
    {
        $stream.Dispose()
    }
}

function Write-NewJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )
    Write-NewText -Path $Path -Text (($Value | ConvertTo-Json -Depth 50) + "`n")
}

function New-Identity
{
    param([Parameter(Mandatory = $true)][string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    return [ordered]@{
        path = [System.IO.Path]::GetFullPath($Path)
        size = [UInt64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function New-TextArtifact
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    Write-NewText -Path $Path -Text $Text
    return New-Identity -Path $Path
}

function Get-ProfileContract
{
    param([Parameter(Mandatory = $true)][string]$ProfileToken)
    switch ($ProfileToken)
    {
        'direct'
        {
            return [ordered]@{
                name = 'Direct-Level 2x2 (Experimental)'
                visualProfileId = [UInt64]::Parse('EBB15DCE41AB436E', [Globalization.NumberStyles]::HexNumber)
                layout = 3
                dataBytes = 86688
                codewords = 42
            }
        }
        'shape'
        {
            return [ordered]@{
                name = 'Shape+Chroma (Experimental)'
                visualProfileId = [UInt64]::Parse('5042534843503031', [Globalization.NumberStyles]::HexNumber)
                layout = 4
                dataBytes = 65016
                codewords = 32
            }
        }
        'remote-lf4'
        {
            return [ordered]@{
                name = 'PB-RemoteVisual-LF4-X1 (Experimental)'
                visualProfileId = [UInt64]::Parse('504252564C463431', [Globalization.NumberStyles]::HexNumber)
                layout = 7
                dataBytes = 8100
                codewords = 4
            }
        }
        default { throw 'Unknown fixture profile' }
    }
}

function Get-ScaleDimensions
{
    param([Parameter(Mandatory = $true)][string]$ScaleTarget)
    switch ($ScaleTarget)
    {
        '0.750' { return [ordered]@{ width = 1440; height = 810 } }
        '1.000' { return [ordered]@{ width = 1920; height = 1080 } }
        '1.259' { return [ordered]@{ width = 2417; height = 1360 } }
        '1.500' { return [ordered]@{ width = 2880; height = 1620 } }
        default { throw 'Unknown fixture scale target' }
    }
}

function Get-FpsPolicy
{
    param([Parameter(Mandatory = $true)][UInt32]$LogicalFps)
    $policy = switch ($LogicalFps)
    {
        1 { [ordered]@{ encoderSeconds = 600; decoderSeconds = 540; noProgressSeconds = 120; replayFps = 2 } }
        2 { [ordered]@{ encoderSeconds = 360; decoderSeconds = 300; noProgressSeconds = 90; replayFps = 4 } }
        5 { [ordered]@{ encoderSeconds = 180; decoderSeconds = 120; noProgressSeconds = 60; replayFps = 10 } }
        default { throw 'Unknown fixture logical FPS' }
    }
    return $policy
}

function New-CombinedReport
{
    param(
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][object]$Profile,
        [Parameter(Mandatory = $true)][UInt32]$LogicalFps,
        [Parameter(Mandatory = $true)][string]$ModeClass,
        [Parameter(Mandatory = $true)][string]$ScaleTarget
    )
    $profileFactor = switch ([string]$Profile.token) { 'direct' { 3.0 }; 'shape' { 2.0 }; default { 1.0 } }
    $observedScale = [double]::Parse($ScaleTarget, [Globalization.CultureInfo]::InvariantCulture)
    $observedGeometry = [ordered]@{
        authority = 'AcceptedBootstrapLocatorPixels'
        samples = [UInt64]12
        lastOriginX = 31.25
        lastOriginY = 47.5
        lastScaleX = $observedScale
        lastScaleY = $observedScale
        lastMarkerResidualPixels = 0.25
        minimumOriginX = 31.0
        maximumOriginX = 31.5
        minimumOriginY = 47.25
        maximumOriginY = 47.75
        minimumScaleX = $observedScale
        maximumScaleX = $observedScale
        minimumScaleY = $observedScale
        maximumScaleY = $observedScale
        minimumMarkerResidualPixels = 0.2
        maximumMarkerResidualPixels = 0.3
        maximumScaleAnisotropy = 0.0
    }
    return [ordered]@{
        schema = 'PixelBridge.RemoteVisualCombinedReport.1'
        runId = $RunId
        profile = [string]$Profile.contract.name
        evidenceValid = $true
        successfulRun = $true
        externalVerification = [ordered]@{ match = $true }
        encoder = [ordered]@{
            state = 'Stopped'
            configuredLogicalVisualFps = $LogicalFps
            generatedVisualFramesPerSecond = [double]$LogicalFps
            generatedPayloadBytesPerSecond = 1000.0 * $profileFactor
        }
        decoder = [ordered]@{
            state = 'Completed'
            captureFps = 30.0
            uniqueVisualFps = [double]$LogicalFps
            endToEndUniqueVisualFps = [double]$LogicalFps
            bootstrapSuccessRate = 0.95
            telemetryBootstrapSuccesses = [UInt64]12
            observedLocatorGeometry = $observedGeometry
            preFecBerEstimate = 0.01 / $profileFactor
            fecFrameErrorRate = 0.02 / $profileFactor
            fecCodewordFailureRate = 0.01 / $profileFactor
            evaluatedDataFrames = [UInt64]12
            evaluatedCodewords = [UInt64]48
            fecAcceptedTransportBlocks = [UInt64]40
            temporallyAdmittedTransportBlocks = [UInt64]36
            remoteMetricTelemetry = [ordered]@{
                frames = [UInt64]12
                samples = [UInt64]1200
                zeroMagnitudeRate = 0.0
                meanAbsoluteMetric = 0.8
                symbolSamples = [UInt64]1000
                unreliableSymbols = [UInt64]2
                unreliableSymbolRate = 0.002
                rejectedFrames = [UInt64]1
                staleRegions = [UInt64]0
                freshnessTagMismatches = [UInt64]0
                freshnessTagErasures = [UInt64]0
            }
            duplicateFrameSequences = [UInt64]1
            reorderedFrameSequences = [UInt64]0
            frameSequenceGapEvents = [UInt64]0
            skippedFrameSequences = [UInt64]0
            captureStall = [ordered]@{ count = [UInt64]0 }
            visualStall = [ordered]@{ count = [UInt64]0 }
            verifiedEncodedGoodputBitsPerSecond = 8000.0 * $profileFactor
            wholeFileDigestVerified = $true
            finalPublishSucceeded = $true
            falseAcceptedCodewords = $null
            outerAdmission = [ordered]@{ conflictRejections = [UInt64]0 }
            remoteMetadata = [ordered]@{
                remoteMode = "Visible-$ModeClass"
                chromaMode = 'Unknown'
                observedLatencyMilliseconds = $null
            }
        }
    }
}

function New-RunFixture
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][UInt32]$Index,
        [Parameter(Mandatory = $true)][ValidateSet('direct', 'shape', 'remote-lf4')][string]$ProfileToken,
        [Parameter(Mandatory = $true)][ValidateSet('wgc', 'dxgi')][string]$Backend,
        [Parameter(Mandatory = $true)][ValidateSet('QualityPriority', 'Automatic', 'Restricted')][string]$ModeClass,
        [Parameter(Mandatory = $true)][ValidateSet(1, 2, 5)][UInt32]$LogicalFps,
        [Parameter(Mandatory = $true)][string]$ScaleTarget,
        [Parameter(Mandatory = $true)][object]$Shared
    )
    $runId = $Index.ToString('x32', [Globalization.CultureInfo]::InvariantCulture)
    $runRoot = Join-Path $Root "run-$runId"
    [void](New-Item -ItemType Directory -Path $runRoot)
    $profileContract = Get-ProfileContract -ProfileToken $ProfileToken
    $profile = [ordered]@{ token = $ProfileToken; contract = $profileContract }
    $targetDimensions = Get-ScaleDimensions -ScaleTarget $ScaleTarget
    if ($ProfileToken -in @('direct', 'shape') -and $ScaleTarget -cne '1.000')
    {
        throw 'Strict fixture profile requested a scaled ROI'
    }
    $captureDimensions = if ($ProfileToken -ceq 'remote-lf4' -and $ScaleTarget -ceq '1.000')
    {
        [ordered]@{ width = 2048; height = 1200 }
    }
    else
    {
        $targetDimensions
    }
    $captureRoiScaleX = [double]$captureDimensions.width / 1920.0
    $captureRoiScaleY = [double]$captureDimensions.height / 1080.0
    $policy = Get-FpsPolicy -LogicalFps $LogicalFps
    $prospectiveFrames = [UInt64]$policy.replayFps * ([UInt64]$policy.decoderSeconds + 2) + 2
    $prospectiveBytes = [UInt64]$captureDimensions.width * [UInt64]$captureDimensions.height * 4 * $prospectiveFrames + 256MB
    if ($prospectiveBytes -gt 16GB)
    {
        $policy.replayFps = $LogicalFps
    }
    $maximumFrames = [UInt32]([UInt64]$policy.replayFps * ([UInt64]$policy.decoderSeconds + 2) + 2)
    $worstCaseBytes = [UInt64]$captureDimensions.width * [UInt64]$captureDimensions.height * 4 * $maximumFrames + 256MB
    $deployment = New-TextArtifact -Path (Join-Path $runRoot 'deployment.json') -Text "deployment-$runId"
    $metadata = New-TextArtifact -Path (Join-Path $runRoot 'metadata.json') -Text "metadata-$runId"
    $ui = New-TextArtifact -Path (Join-Path $runRoot 'ui.json') -Text "ui-$runId"
    $encoderEnvironment = New-TextArtifact -Path (Join-Path $runRoot 'encoder-environment.json') -Text "encoder-env-$runId"
    $decoderEnvironment = New-TextArtifact -Path (Join-Path $runRoot 'decoder-environment.json') -Text "decoder-env-$runId"
    $encoderReport = New-TextArtifact -Path (Join-Path $runRoot 'encoder-report.json') -Text "encoder-report-$runId"
    $liveReport = New-TextArtifact -Path (Join-Path $runRoot 'live-report.json') -Text "live-report-$runId"
    $offlineReport = New-TextArtifact -Path (Join-Path $runRoot 'offline-report.json') -Text "offline-report-$runId"
    $replay = New-TextArtifact -Path (Join-Path $runRoot 'capture.pbrv2') -Text "replay-$runId"
    $combinedPath = Join-Path $runRoot 'combined.json'
    Write-NewJson -Path $combinedPath -Value (New-CombinedReport -RunId $runId -Profile $profile `
        -LogicalFps $LogicalFps -ModeClass $ModeClass -ScaleTarget $ScaleTarget)
    $protectedRect = [ordered]@{ left = 0; top = 0; right = 1920; bottom = 1200 }
    $experimentRect = [ordered]@{ left = 2000; top = 0; right = 5840; bottom = 2160 }
    $protectedContract = [ordered]@{
        deviceName = 'P'; physicalRect = $protectedRect; dpiX = 96; dpiY = 96; refreshRate = 60
        rotation = 'Identity'; adapterLuid = [ordered]@{ high = 0; low = 1 }; primary = $true
    }
    $experimentContract = [ordered]@{
        deviceName = 'E'; physicalRect = $experimentRect; dpiX = 96; dpiY = 96; refreshRate = 60
        rotation = 'Identity'; adapterLuid = [ordered]@{ high = 0; low = 1 }; primary = $false
    }
    $encoderEnforcement = if ($ProfileToken -ceq 'remote-lf4')
    {
        'ProductionRuntimePreflightAndPeriodicRevalidation'
    }
    else
    {
        'PlanAndEndpointEnvironmentContainment; current Direct/Shape Encoder CLI does not accept monitor-safety arguments'
    }
    $plan = [ordered]@{
        schema = 'PixelBridge.RemoteVisualPilotPlan.3'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        runId = $runId
        profileToken = $ProfileToken
        profileName = [string]$profileContract.name
        visualProfileId = [UInt64]$profileContract.visualProfileId
        visualLayoutVersion = [UInt32]$profileContract.layout
        codedDataBytesPerFrame = [UInt32]$profileContract.dataBytes
        codewordsPerFrame = [UInt32]$profileContract.codewords
        logicalFps = $LogicalFps
        geometryMode = if ($ScaleTarget -ceq '1.000') { 'Strict1To1' } else { 'LocatorScaled' }
        deployment = [ordered]@{
            runId = $runId
            manifest = $deployment
            packageManifest = $Shared.package
            sourceManifest = $Shared.sourceManifest
            remoteMetadata = $metadata
            encoderEnvironment = $encoderEnvironment
            decoderEnvironment = $decoderEnvironment
        }
        applications = [ordered]@{
            encoder = [ordered]@{ role = 'Encoder'; relativeExecutablePath = 'Encoder/PixelBridgeEncoder.exe'; size = 1; sha256 = '1' * 64; versionOutput = 'fixture' }
            decoder = [ordered]@{ role = 'Decoder'; relativeExecutablePath = 'Decoder/PixelBridgeDecoder.exe'; size = 1; sha256 = '2' * 64; versionOutput = 'fixture' }
        }
        source = [ordered]@{
            sourceSetId = '0' * 32
            relativePath = 'random-1MiB.bin'
            size = [UInt64]$Shared.source.size
            sha256 = [string]$Shared.source.sha256
            pixelBridgeSegmentCompression = 'RAW/OFF'
        }
        remoteUi = [ordered]@{
            runId = $runId
            provenance = 'RemoteUiVisible'
            evidence = $ui
            screenshot = $Shared.screenshot
            captureRecord = $Shared.captureRecord
            visibleFields = @('remoteProvider', 'remoteMode')
            visibleClaims = [ordered]@{ remoteProvider = 'ContractFixture'; remoteMode = "Visible-$ModeClass" }
        }
        monitorSafety = [ordered]@{
            encoder = [ordered]@{
                protectedMonitorDeviceName = 'P'; experimentMonitorDeviceName = 'E'
                protectedMonitorPhysicalRect = $protectedRect; experimentMonitorPhysicalRect = $experimentRect
                dataWindowPhysicalRect = [ordered]@{ left = 2100; top = 100; right = 4020; bottom = 1180 }
                runtimeEnforcement = $encoderEnforcement
                protectedMonitorContract = $protectedContract; experimentMonitorContract = $experimentContract
            }
            decoder = [ordered]@{
                protectedMonitorDeviceName = 'P'; experimentMonitorDeviceName = 'E'
                protectedMonitorPhysicalRect = $protectedRect; experimentMonitorPhysicalRect = $experimentRect
                roiPhysicalRect = [ordered]@{ left = 2000; top = 0; right = 2000 + $captureDimensions.width; bottom = $captureDimensions.height }
                runtimeEnforcement = 'ProductionRuntimePreflightAndPeriodicRevalidation'
                protectedMonitorContract = $protectedContract; experimentMonitorContract = $experimentContract
            }
        }
        remoteGeometry = [ordered]@{
            captureRoiWidth = [UInt32]$captureDimensions.width
            captureRoiHeight = [UInt32]$captureDimensions.height
            captureRoiScaleX = $captureRoiScaleX
            captureRoiScaleY = $captureRoiScaleY
            expectedLocatorScaleTarget = $ScaleTarget
            scaleAuthority = 'AcceptedBootstrapLocatorPixels'
            axisAligned = $true
            cropStatus = 'NoneExpected'
            locatorRemainsAuthoritative = $true
        }
        policy = [ordered]@{
            captureBackend = $Backend; compression = 'off'
            encoderHardMaximumSeconds = [UInt32]$policy.encoderSeconds
            decoderTimeoutSeconds = [UInt32]$policy.decoderSeconds
            noProgressSeconds = [UInt32]$policy.noProgressSeconds
            offlineReplayTimeoutSeconds = 600; offlineNoProgressSeconds = 120; decoderWarmupMaximumSeconds = 30
            controlRepetitions = 12; postPublishReplayTailSeconds = 2; postDecoderBroadcastProofSeconds = 7
            manualStopRequired = $true; decoderStartsBeforeEncoder = $true; noFileClipboardOrIpcSideChannel = $true
            replay = [ordered]@{
                maximumCaptureFramesPerSecond = [UInt32]$policy.replayFps
                maximumFrames = $maximumFrames
                maximumMiB = 16384
                reservedNonFrameMiB = 256
                worstCaseSampledFrames = [UInt64]$maximumFrames
                worstCaseBytesIncludingReserve = $worstCaseBytes
            }
            clock = [ordered]@{ decoderOffsetMilliseconds = 0; uncertaintyMilliseconds = 5000 }
        }
        matrix = [ordered]@{
            modeClass = $ModeClass
            visibleRemoteMode = "Visible-$ModeClass"
            scaleTarget = $ScaleTarget
            profileComparisonRole = if ($ProfileToken -ceq 'remote-lf4') { 'Candidate' } else { 'Baseline' }
            backendCoverageRole = if ($Backend -ceq 'wgc') { 'MainMatrix' } else { 'RepresentativeRecheck' }
            runIsolation = 'IndependentRunIdAndArtifacts; metrics from different runs must not be merged'
        }
    }
    $planPath = Join-Path $runRoot 'plan.json'
    Write-NewJson -Path $planPath -Value $plan
    $frozenPlan = Import-PBRemoteVisualPilotPlan -Path $planPath
    $verificationPath = Join-Path $runRoot 'verification.json'
    Write-NewJson -Path $verificationPath -Value ([ordered]@{
        schema = 'PixelBridge.RemoteVisualPilotEvidenceVerification.1'
        status = 'PASS'
        runId = $runId
        profileToken = $ProfileToken
        captureBackend = $Backend
        matrix = $plan.matrix
        completion = [ordered]@{ failureClassification = 'success' }
        plan = $frozenPlan.identity
        replayIdentity = $replay
        combinedReport = New-Identity -Path $combinedPath
        endpointReports = [ordered]@{
            encoder = $encoderReport
            liveDecoder = $liveReport
            offlineDecoder = $offlineReport
        }
    })
    $record = New-PBRemoteVisualMatrixRunRecordValue -FrozenPlan $frozenPlan -Outcome Success `
        -FailureClassification success -SourcePath $Shared.source.path -ReplayPath $replay.path `
        -CombinedReportPath $combinedPath -EncoderReportPath $encoderReport.path -LiveDecoderReportPath $liveReport.path `
        -OfflineDecoderReportPath $offlineReport.path -OutcomeVerificationPath $verificationPath -InspectionPath $null
    $recordPath = Join-Path $runRoot 'matrix-run-record.json'
    Write-NewJson -Path $recordPath -Value $record
    [void](Import-PBRemoteVisualMatrixRunRecord -Path $recordPath)
    $artifactList = [Collections.Generic.List[object]]::new()
    $artifactPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $recordIdentity = New-Identity -Path $recordPath
    [void]$artifactList.Add($recordIdentity)
    [void]$artifactPaths.Add([string]$recordIdentity.path)
    foreach ($identity in $record.identities.Values)
    {
        if ($null -ne $identity -and $artifactPaths.Add([string]$identity.path))
        {
            [void]$artifactList.Add($identity)
        }
    }
    $sealPath = Join-Path $runRoot 'pilot-evidence-seal.json'
    Write-NewJson -Path $sealPath -Value ([ordered]@{
        schema = 'PixelBridge.RemoteVisualPilotEvidenceSeal.1'
        status = 'PASS'
        runId = $runId
        artifactCount = $artifactList.Count
        artifacts = @($artifactList)
    })
    return $runRoot
}

$resolvedToolsRoot = [System.IO.Path]::GetFullPath($ToolsRoot)
$resolvedWorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
$matrixTool = Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualStep21Matrix.ps1'
$matrixModule = Join-Path $resolvedToolsRoot 'PBRemoteVisualMatrixCommon.psm1'
$matrixSpecTool = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualStep21MatrixSpec.ps1'
$hardwareScopeTool = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualStep21HardwareScope.ps1'
$runLedgerTool = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualStep21RunLedger.ps1'
foreach ($path in @($matrixTool, $matrixModule, $matrixSpecTool, $hardwareScopeTool, $runLedgerTool,
    (Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualFieldFailureEvidence.ps1')))
{
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Required Step 21 evidence tool is missing: $path"
    }
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0)
    {
        throw "PowerShell parser rejected $path`: $($errors[0].Message)"
    }
}
if (-not (Test-Path -LiteralPath $resolvedWorkRoot -PathType Container))
{
    [void](New-Item -ItemType Directory -Path $resolvedWorkRoot)
}
$runRoot = Join-Path $resolvedWorkRoot ("run-" + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $runRoot)
try
{
    Import-Module -Name (Join-Path $resolvedToolsRoot 'PBRemoteVisualPilotCommon.psm1') -Force -ErrorAction Stop
    Import-Module -Name $matrixModule -Force -ErrorAction Stop
    $matrixSpecPath = Join-Path $runRoot 'matrix-spec.json'
    & $matrixSpecTool -ComparisonModeClass QualityPriority -ComparisonLogicalFps 5 -OutputPath $matrixSpecPath | Out-Null
    $matrixSpecIdentity = New-Identity -Path $matrixSpecPath
    $monitorCatalogPath = Join-Path $runRoot 'computer-a-monitor-catalog.json'
    Write-NewJson -Path $monitorCatalogPath -Value ([ordered]@{
        schema = 'PixelBridge.MonitorCatalog.1'
        monitorCount = 2
        monitors = @(
            [ordered]@{
                deviceName = '\\.\DISPLAY1'
                physicalRect = [ordered]@{ left = 0; top = 0; right = 2560; bottom = 1440 }
                resolution = [ordered]@{ width = 2560; height = 1440 }
                rotation = 'Identity'
            },
            [ordered]@{
                deviceName = '\\.\DISPLAY2'
                physicalRect = [ordered]@{ left = 2560; top = 0; right = 5120; bottom = 1440 }
                resolution = [ordered]@{ width = 2560; height = 1440 }
                rotation = 'Identity'
            })
    })
    $monitorCatalogIdentity = New-Identity -Path $monitorCatalogPath
    $overlappingCatalog = Read-PBBoundedJson -Path $monitorCatalogPath -MaximumBytes 2MB
    $overlappingCatalog.monitors[1].physicalRect.left = 0
    $overlappingCatalog.monitors[1].physicalRect.right = 2560
    $overlappingCatalogPath = Join-Path $runRoot 'computer-a-monitor-catalog-overlap.json'
    Write-NewJson -Path $overlappingCatalogPath -Value $overlappingCatalog
    $overlappingCatalogIdentity = New-Identity -Path $overlappingCatalogPath
    $invalidScopeOutput = Join-Path $runRoot 'invalid-hardware-scope.json'
    $overlapRejected = $false
    try
    {
        & $hardwareScopeTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -ComputerAMonitorCatalogPath $overlappingCatalogPath `
            -ExpectedComputerAMonitorCatalogSha256 $overlappingCatalogIdentity.sha256 `
            -ExperimentMonitorDeviceName '\\.\DISPLAY2' -OutputPath $invalidScopeOutput | Out-Null
    }
    catch { $overlapRejected = $_.Exception.Message -like '*non-overlapping*' }
    if (-not $overlapRejected -or (Test-Path -LiteralPath $invalidScopeOutput) -or
        (Test-Path -LiteralPath "$invalidScopeOutput.partial") -or
        @(Get-ChildItem -LiteralPath $runRoot -Filter 'invalid-hardware-scope.json.validation-*.json' -Force).Count -ne 0)
    {
        throw 'Step 21 hardware scope did not reject overlapping monitors before publishing any final or temporary output'
    }
    $hardwareScopePath = Join-Path $runRoot 'hardware-scope.json'
    & $hardwareScopeTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
        -ComputerAMonitorCatalogPath $monitorCatalogPath `
        -ExpectedComputerAMonitorCatalogSha256 $monitorCatalogIdentity.sha256 `
        -ExperimentMonitorDeviceName '\\.\DISPLAY2' -OutputPath $hardwareScopePath | Out-Null
    $hardwareScopeIdentity = New-Identity -Path $hardwareScopePath

    $runLedgerOutput = Join-Path $runRoot 'run-ledger'
    & $runLedgerTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
        -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 $hardwareScopeIdentity.sha256 `
        -OutputDirectory $runLedgerOutput | Out-Null
    $runLedgerPath = Join-Path $runLedgerOutput 'step21-run-ledger.json'
    $runLedgerIdentity = New-Identity -Path $runLedgerPath
    $runLedgerImport = Import-PBRemoteVisualStep21RunLedger -Path $runLedgerPath `
        -ExpectedSha256 $runLedgerIdentity.sha256
    $runLedger = $runLedgerImport.value
    $lf4OneToOne = @($runLedger.entries | Where-Object {
        [string]$_.cellId -ceq 'lf4-wgc-quality-priority-s1000-f1'
    })
    $lf4Scaled = @($runLedger.entries | Where-Object {
        [string]$_.cellId -ceq 'lf4-wgc-quality-priority-s0750-f1'
    })
    $directBaseline = @($runLedger.entries | Where-Object {
        [string]$_.profileToken -ceq 'direct'
    })
    if ([string]$runLedger.schema -cne 'PixelBridge.RemoteVisualStep21RunLedger.1' -or
        [string]$runLedger.status -cne 'NOT_EXECUTED' -or @($runLedger.entries).Count -ne 31 -or
        [UInt32]$runLedger.truthBoundary.executedCellCount -ne 0 -or
        [UInt32]$runLedger.truthBoundary.pendingCellCount -ne 31 -or
        [bool]$runLedger.truthBoundary.formalStep21Accepted -or
        $lf4OneToOne.Count -ne 1 -or [string]$lf4OneToOne[0].geometryMode -cne 'Strict1To1' -or
        [UInt32]$lf4OneToOne[0].minimumTargetCanvas.width -ne 1920 -or
        [UInt32]$lf4OneToOne[0].minimumTargetCanvas.height -ne 1080 -or
        [Int64]$lf4OneToOne[0].decoderCaptureRoi.left -ne 2560 -or
        [Int64]$lf4OneToOne[0].decoderCaptureRoi.top -ne 0 -or
        [Int64]$lf4OneToOne[0].decoderCaptureRoi.right -ne 5120 -or
        [Int64]$lf4OneToOne[0].decoderCaptureRoi.bottom -ne 1440 -or
        [string]$lf4OneToOne[0].decoderCaptureRoiArgument -cne '2560,0,2560,1440' -or
        [string]$lf4OneToOne[0].captureRoiPurpose -cne 'LocatorSearchNeighborhood' -or
        $lf4Scaled.Count -ne 1 -or [string]$lf4Scaled[0].geometryMode -cne 'LocatorScaled' -or
        $directBaseline.Count -ne 1 -or [string]$directBaseline[0].captureRoiPurpose -cne 'ExactProfileCanvas' -or
        [string]$directBaseline[0].decoderCaptureRoiArgument -cne '2880,180,1920,1080' -or
        $null -ne $directBaseline[0].runId -or [string]$directBaseline[0].executionStatus -cne 'PENDING')
    {
        throw 'Step 21 run ledger did not freeze the exact 31-cell operator schedule and ROI/target distinction'
    }
    $runLedgerRaw = [System.IO.File]::ReadAllText($runLedgerPath)
    $runLedgerCsvPath = Join-Path $runLedgerOutput 'step21-run-ledger.csv'
    $runLedgerCsv = @(Import-Csv -LiteralPath $runLedgerCsvPath)
    if ($runLedgerRaw.Contains('estimatedScaleX') -or $runLedgerRaw.Contains('estimatedScaleY') -or
        $runLedgerCsv.Count -ne 31 -or $runLedgerCsv[0].PSObject.Properties.Name -contains 'estimatedScaleX' -or
        $runLedgerCsv[0].PSObject.Properties.Name -contains 'estimatedScaleY')
    {
        throw 'Step 21 run ledger retained a legacy ROI-derived scale field or the wrong CSV row count'
    }
    $runLedgerSealPath = Join-Path $runLedgerOutput 'step21-run-ledger.seal.json'
    $runLedgerSeal = Read-PBBoundedJson -Path $runLedgerSealPath -MaximumBytes 512KB
    if ([string]$runLedgerSeal.schema -cne 'PixelBridge.RemoteVisualStep21RunLedgerSeal.1' -or
        [string]$runLedgerSeal.status -cne 'NOT_EXECUTED' -or [UInt32]$runLedgerSeal.artifactCount -ne 5 -or
        @($runLedgerSeal.artifacts).Count -ne 5 -or [bool]$runLedgerSeal.formalStep21Accepted -or
        [bool]$runLedgerSeal.certifiedRemoteVisualProfile)
    {
        throw 'Step 21 run-ledger seal did not preserve the non-execution truth boundary'
    }
    foreach ($artifact in @($runLedgerSeal.artifacts))
    {
        $actualArtifact = New-Identity -Path ([string]$artifact.path)
        if ([UInt64]$actualArtifact.size -ne [UInt64]$artifact.size -or
            [string]$actualArtifact.sha256 -cne [string]$artifact.sha256)
        {
            throw 'Step 21 run-ledger seal contains a stale artifact identity'
        }
    }

    $runLedgerTampered = Read-PBBoundedJson -Path $runLedgerPath -MaximumBytes 2MB
    $runLedgerTampered.entries[0].decoderCaptureRoi.right = 5119
    $runLedgerTamperedPath = Join-Path $runRoot 'run-ledger-tampered.json'
    Write-NewJson -Path $runLedgerTamperedPath -Value $runLedgerTampered
    $runLedgerTamperedIdentity = New-Identity -Path $runLedgerTamperedPath
    $runLedgerTamperRejected = $false
    try
    {
        [void](Import-PBRemoteVisualStep21RunLedger -Path $runLedgerTamperedPath `
            -ExpectedSha256 $runLedgerTamperedIdentity.sha256)
    }
    catch { $runLedgerTamperRejected = $_.Exception.Message -like '*run-ledger entry*' }
    if (-not $runLedgerTamperRejected)
    {
        throw 'Step 21 run ledger allowed post-generation capture ROI tampering'
    }

    $runLedgerHashMismatchOutput = Join-Path $runRoot 'run-ledger-hash-mismatch'
    $runLedgerHashMismatchRejected = $false
    try
    {
        & $runLedgerTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 ('0' * 64) `
            -OutputDirectory $runLedgerHashMismatchOutput | Out-Null
    }
    catch { $runLedgerHashMismatchRejected = $_.Exception.Message -like '*expected SHA-256*' }
    if (-not $runLedgerHashMismatchRejected -or (Test-Path -LiteralPath $runLedgerHashMismatchOutput))
    {
        throw 'Step 21 run-ledger tool published output after a HardwareScope identity mismatch'
    }

    $runLedgerDuplicateRejected = $false
    try
    {
        & $runLedgerTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 $hardwareScopeIdentity.sha256 `
            -OutputDirectory $runLedgerOutput | Out-Null
    }
    catch { $runLedgerDuplicateRejected = $_.Exception.Message -like '*must be a new directory*' }
    if (-not $runLedgerDuplicateRejected)
    {
        throw 'Step 21 run-ledger tool overwrote or reused an existing output directory'
    }

    $sourcePath = Join-Path $runRoot 'random-1MiB.bin'
    [System.IO.File]::WriteAllBytes($sourcePath, [byte[]]::new(1MB))
    $shared = [ordered]@{
        source = New-Identity -Path $sourcePath
        package = New-TextArtifact -Path (Join-Path $runRoot 'package-manifest.json') -Text 'shared-package'
        sourceManifest = New-TextArtifact -Path (Join-Path $runRoot 'source-manifest.json') -Text 'shared-source-manifest'
        screenshot = New-TextArtifact -Path (Join-Path $runRoot 'remote-ui.png') -Text 'visible-ui-pixels'
        captureRecord = New-TextArtifact -Path (Join-Path $runRoot 'capture-record.json') -Text 'capture-record'
    }
    $directories = [Collections.Generic.List[string]]::new()
    $index = [UInt32]1
    foreach ($mode in @('QualityPriority', 'Automatic', 'Restricted'))
    {
        foreach ($scale in @('0.750', '1.000', '1.259', '1.500'))
        {
            foreach ($fps in @(1, 2, 5))
            {
                [void]$directories.Add((New-RunFixture -Root $runRoot -Index $index -ProfileToken remote-lf4 `
                    -Backend wgc -ModeClass $mode -LogicalFps $fps -ScaleTarget $scale -Shared $shared))
                $index++
            }
        }
    }
    foreach ($case in @(
        [ordered]@{ mode = 'QualityPriority'; fps = 1; scale = '0.750' },
        [ordered]@{ mode = 'Automatic'; fps = 2; scale = '1.259' },
        [ordered]@{ mode = 'Restricted'; fps = 5; scale = '1.500' }))
    {
        [void]$directories.Add((New-RunFixture -Root $runRoot -Index $index -ProfileToken remote-lf4 `
            -Backend dxgi -ModeClass $case.mode -LogicalFps $case.fps -ScaleTarget $case.scale -Shared $shared))
        $index++
    }
    foreach ($profileToken in @('direct', 'shape'))
    {
        [void]$directories.Add((New-RunFixture -Root $runRoot -Index $index -ProfileToken $profileToken `
            -Backend wgc -ModeClass QualityPriority -LogicalFps 5 -ScaleTarget '1.000' -Shared $shared))
        $index++
    }
    if ($directories.Count -ne 41)
    {
        throw 'Step 21 contract fixture did not create the expected 41 independent runs'
    }

    $arbitraryAspectRun = @($directories | Where-Object {
        $candidate = Read-PBBoundedJson -Path (Join-Path $_ 'matrix-run-record.json') -MaximumBytes 4MB
        [string]$candidate.profileToken -ceq 'remote-lf4' -and [string]$candidate.matrix.scaleTarget -ceq '1.000'
    })[0]
    $arbitraryAspectPlan = Read-PBBoundedJson -Path (Join-Path $arbitraryAspectRun 'plan.json') -MaximumBytes 256KB
    $arbitraryAspectRecord = Read-PBBoundedJson -Path (Join-Path $arbitraryAspectRun 'matrix-run-record.json') -MaximumBytes 4MB
    if ([UInt32]$arbitraryAspectPlan.remoteGeometry.captureRoiWidth -ne 2048 -or
        [UInt32]$arbitraryAspectPlan.remoteGeometry.captureRoiHeight -ne 1200 -or
        [Math]::Abs([double]$arbitraryAspectPlan.remoteGeometry.captureRoiScaleX - 1.0) -lt 0.01 -or
        [Math]::Abs([double]$arbitraryAspectRecord.matrix.observedLocatorGeometry.lastScaleX - 1.0) -gt 0.000001 -or
        $arbitraryAspectRecord.matrix.Contains('estimatedScaleX') -or
        [string]$arbitraryAspectRecord.contracts.geometryAuthority -cne 'AcceptedBootstrapLocatorPixels')
    {
        throw 'Step 21 fixture did not prove arbitrary-aspect capture ROI separation from accepted Locator scale truth'
    }

    $output = Join-Path $runRoot 'matrix-output'
    & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
        -RunEvidenceDirectory @($directories) -OutputDirectory $output | Out-Null
    $summaryPath = Join-Path $output 'step21-provider-generic-matrix.json'
    $summary = Read-PBBoundedJson -Path $summaryPath -MaximumBytes 8MB
    if ([string]$summary.schema -cne 'PixelBridge.RemoteVisualStep21MatrixEvidence.3' -or
        [string]$summary.status -cne 'PASS' -or [UInt32]$summary.runCount -ne 41 -or
        [UInt32]$summary.coverage.lf4WgcMain.exactCells -ne 36 -or
        [UInt32]$summary.coverage.lf4DxgiRepresentative.runCount -ne 3 -or
        $summary.truthBoundary.captureRoiIsSearchNeighborhoodNotScaleEvidence -isnot [bool] -or
        -not [bool]$summary.truthBoundary.captureRoiIsSearchNeighborhoodNotScaleEvidence -or
        [string]$summary.truthBoundary.acceptedLocatorGeometryAuthority -cne 'AcceptedBootstrapLocatorPixels' -or
        [string]::IsNullOrWhiteSpace([string]$summary.authoritativeComparison.tupleKey))
    {
        throw 'Step 21 positive matrix fixture did not publish the exact coverage/comparison result'
    }

    $hardwareScopedDirectories = @($directories | Where-Object {
        $record = Read-PBBoundedJson -Path (Join-Path $_ 'matrix-run-record.json') -MaximumBytes 4MB
        [string]$record.matrix.scaleTarget -cne '1.500'
    })
    if ($hardwareScopedDirectories.Count -ne 31)
    {
        throw 'Step 21 contract fixture did not derive the exact 31 hardware-scoped runs'
    }
    $hardwareScopedOutput = Join-Path $runRoot 'matrix-output-hardware-scoped'
    & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
        -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 $hardwareScopeIdentity.sha256 `
        -RunEvidenceDirectory $hardwareScopedDirectories -OutputDirectory $hardwareScopedOutput | Out-Null
    $hardwareScopedSummary = Read-PBBoundedJson `
        -Path (Join-Path $hardwareScopedOutput 'step21-provider-generic-matrix.json') -MaximumBytes 8MB
    if ([string]$hardwareScopedSummary.schema -cne 'PixelBridge.RemoteVisualStep21MatrixEvidence.3' -or
        [string]$hardwareScopedSummary.status -cne 'PASS' -or [UInt32]$hardwareScopedSummary.runCount -ne 31 -or
        [UInt32]$hardwareScopedSummary.coverage.lf4WgcMain.exactCells -ne 27 -or
        [UInt32]$hardwareScopedSummary.coverage.lf4DxgiRepresentative.runCount -ne 2 -or
        [string]$hardwareScopedSummary.matrixSpecification.hardwareScope.scopeId -cnotmatch '^[0-9a-f]{32}$' -or
        [UInt32]$hardwareScopedSummary.matrixSpecification.hardwareScope.excludedCellCount -ne 10 -or
        $hardwareScopedSummary.truthBoundary.hardwareScoped -isnot [bool] -or
        -not [bool]$hardwareScopedSummary.truthBoundary.hardwareScoped -or
        $hardwareScopedSummary.truthBoundary.full41CoverageCompleted -isnot [bool] -or
        [bool]$hardwareScopedSummary.truthBoundary.full41CoverageCompleted -or
        $hardwareScopedSummary.truthBoundary.excludedCellsDoNotCountAsCoverage -isnot [bool] -or
        -not [bool]$hardwareScopedSummary.truthBoundary.excludedCellsDoNotCountAsCoverage)
    {
        throw 'Step 21 hardware-scoped fixture did not publish the exact 31/10 truth boundary'
    }

    $hardwareScopeTampered = Read-PBBoundedJson -Path $hardwareScopePath -MaximumBytes 512KB
    $hardwareScopeTampered.includedCells[0].cellId = 'tampered-cell-id'
    $hardwareScopeTamperedPath = Join-Path $runRoot 'hardware-scope-tampered.json'
    Write-NewJson -Path $hardwareScopeTamperedPath -Value $hardwareScopeTampered
    $hardwareScopeTamperedIdentity = New-Identity -Path $hardwareScopeTamperedPath
    $hardwareScopeTamperRejected = $false
    try
    {
        [void](Import-PBRemoteVisualStep21HardwareScope -Path $hardwareScopeTamperedPath `
            -ExpectedSha256 $hardwareScopeTamperedIdentity.sha256)
    }
    catch { $hardwareScopeTamperRejected = $_.Exception.Message -like '*included cells*' }
    if (-not $hardwareScopeTamperRejected)
    {
        throw 'Step 21 hardware scope allowed post-generation cell selection tampering'
    }

    $hardwareScopeFractional = Read-PBBoundedJson -Path $hardwareScopePath -MaximumBytes 512KB
    $hardwareScopeFractional.includedCellCount = 31.5
    $hardwareScopeFractionalPath = Join-Path $runRoot 'hardware-scope-fractional-count.json'
    Write-NewJson -Path $hardwareScopeFractionalPath -Value $hardwareScopeFractional
    $hardwareScopeFractionalIdentity = New-Identity -Path $hardwareScopeFractionalPath
    $hardwareScopeFractionalRejected = $false
    try
    {
        [void](Import-PBRemoteVisualStep21HardwareScope -Path $hardwareScopeFractionalPath `
            -ExpectedSha256 $hardwareScopeFractionalIdentity.sha256)
    }
    catch { $hardwareScopeFractionalRejected = $_.Exception.Message -like '*non-negative integer*' }
    if (-not $hardwareScopeFractionalRejected)
    {
        throw 'Step 21 hardware scope silently rounded a fractional cell count'
    }

    $matrixSpecFractional = Read-PBBoundedJson -Path $matrixSpecPath -MaximumBytes 256KB
    $matrixSpecFractional.cellCount = 41.5
    $matrixSpecFractionalPath = Join-Path $runRoot 'matrix-spec-fractional-count.json'
    Write-NewJson -Path $matrixSpecFractionalPath -Value $matrixSpecFractional
    $matrixSpecFractionalIdentity = New-Identity -Path $matrixSpecFractionalPath
    $matrixSpecFractionalRejected = $false
    try
    {
        [void](Import-PBRemoteVisualStep21MatrixSpec -Path $matrixSpecFractionalPath `
            -ExpectedSha256 $matrixSpecFractionalIdentity.sha256)
    }
    catch { $matrixSpecFractionalRejected = $_.Exception.Message -like '*non-negative integer*' }
    if (-not $matrixSpecFractionalRejected)
    {
        throw 'Step 21 MatrixSpec silently rounded a fractional cell count'
    }

    $hardwareScopedMissingOutput = Join-Path $runRoot 'matrix-hardware-scoped-missing-cell'
    $hardwareScopedMissingRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 $hardwareScopeIdentity.sha256 `
            -RunEvidenceDirectory @($hardwareScopedDirectories | Select-Object -Skip 1) `
            -OutputDirectory $hardwareScopedMissingOutput | Out-Null
    }
    catch { $hardwareScopedMissingRejected = $_.Exception.Message -like '*exactly 31 run records*' }
    if (-not $hardwareScopedMissingRejected)
    {
        throw 'Step 21 hardware-scoped matrix did not reject a missing included cell'
    }

    $hardwareScopedOutOfScopeOutput = Join-Path $runRoot 'matrix-hardware-scoped-out-of-scope-substitution'
    $hardwareScopedSubstitution = @($hardwareScopedDirectories)
    $excludedDirectory = @($directories | Where-Object {
        $record = Read-PBBoundedJson -Path (Join-Path $_ 'matrix-run-record.json') -MaximumBytes 4MB
        [string]$record.matrix.scaleTarget -ceq '1.500'
    })[0]
    $hardwareScopedSubstitution[0] = $excludedDirectory
    $hardwareScopedOutOfScopeRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 $hardwareScopeIdentity.sha256 `
            -RunEvidenceDirectory $hardwareScopedSubstitution -OutputDirectory $hardwareScopedOutOfScopeOutput | Out-Null
    }
    catch { $hardwareScopedOutOfScopeRejected = $_.Exception.Message -like '*expected exactly one independent run*' }
    if (-not $hardwareScopedOutOfScopeRejected)
    {
        throw 'Step 21 hardware-scoped matrix allowed an excluded 1.5x run to replace an included cell'
    }

    $hardwareScopedFullInputOutput = Join-Path $runRoot 'matrix-hardware-scoped-full-input'
    $hardwareScopedFullInputRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -HardwareScopePath $hardwareScopePath -ExpectedHardwareScopeSha256 $hardwareScopeIdentity.sha256 `
            -RunEvidenceDirectory @($directories) -OutputDirectory $hardwareScopedFullInputOutput | Out-Null
    }
    catch { $hardwareScopedFullInputRejected = $_.Exception.Message -like '*exactly 31 run records*' }
    if (-not $hardwareScopedFullInputRejected)
    {
        throw 'Step 21 hardware-scoped matrix did not enforce its exact included-run count'
    }

    $hardwareScopePairOutput = Join-Path $runRoot 'matrix-hardware-scope-pair-mismatch'
    $hardwareScopePairRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -HardwareScopePath $hardwareScopePath -RunEvidenceDirectory $hardwareScopedDirectories `
            -OutputDirectory $hardwareScopePairOutput | Out-Null
    }
    catch { $hardwareScopePairRejected = $_.Exception.Message -like '*must be supplied together*' }
    if (-not $hardwareScopePairRejected)
    {
        throw 'Step 21 matrix accepted an unsealed hardware scope path without its expected SHA-256'
    }

    Add-Content -LiteralPath $monitorCatalogPath -Value ' ' -NoNewline
    $hardwareScopeCatalogTamperRejected = $false
    try
    {
        [void](Import-PBRemoteVisualStep21HardwareScope -Path $hardwareScopePath `
            -ExpectedSha256 $hardwareScopeIdentity.sha256)
    }
    catch { $hardwareScopeCatalogTamperRejected = $_.Exception.Message -like '*identity mismatch*' }
    if (-not $hardwareScopeCatalogTamperRejected)
    {
        throw 'Step 21 hardware scope did not bind the Computer A monitor catalog identity'
    }

    $createOnlyRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -RunEvidenceDirectory @($directories) -OutputDirectory $output | Out-Null
    }
    catch { $createOnlyRejected = $_.Exception.Message -like '*new path*' }
    if (-not $createOnlyRejected)
    {
        throw 'Step 21 matrix did not reject an existing output directory'
    }

    $containedOutput = Join-Path $directories[0] 'matrix-output-inside-sealed-run'
    $containedOutputRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -RunEvidenceDirectory @($directories) -OutputDirectory $containedOutput | Out-Null
    }
    catch { $containedOutputRejected = $_.Exception.Message -like '*outside every sealed run*' }
    if (-not $containedOutputRejected)
    {
        throw 'Step 21 matrix allowed its aggregate output to mutate a sealed run evidence root'
    }

    $missingCellOutput = Join-Path $runRoot 'matrix-missing-cell'
    $missingCellRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -RunEvidenceDirectory @($directories | Select-Object -Skip 1) -OutputDirectory $missingCellOutput | Out-Null
    }
    catch { $missingCellRejected = $_.Exception.Message -like '*exactly 41 run records*' }
    if (-not $missingCellRejected)
    {
        throw 'Step 21 matrix did not reject a missing LF4/WGC Cartesian cell'
    }

    $duplicateOutput = Join-Path $runRoot 'matrix-duplicate-run'
    $duplicateDirectories = @($directories)
    $duplicateDirectories[$duplicateDirectories.Count - 1] = $directories[0]
    $duplicateRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -RunEvidenceDirectory $duplicateDirectories -OutputDirectory $duplicateOutput | Out-Null
    }
    catch { $duplicateRejected = $_.Exception.Message -like '*repeats an evidence directory*' }
    if (-not $duplicateRejected)
    {
        throw 'Step 21 matrix did not reject a reused run evidence directory'
    }

    $fractionalCounterReport = New-CombinedReport -RunId ('f' * 32) `
        -Profile ([ordered]@{ token = 'remote-lf4'; contract = Get-ProfileContract -ProfileToken remote-lf4 }) `
        -LogicalFps 5 -ModeClass QualityPriority -ScaleTarget '1.000'
    $fractionalCounterReport.decoder.evaluatedDataFrames = 1.5
    $fractionalCounterRejected = $false
    try
    {
        [void](Get-PBRemoteVisualAuthoritativeMetrics -CombinedReport $fractionalCounterReport)
    }
    catch { $fractionalCounterRejected = $_.Exception.Message -like '*non-negative integer*' }
    if (-not $fractionalCounterRejected)
    {
        throw 'Step 21 authoritative metrics silently rounded a fractional counter'
    }

    $zeroGeometryReport = New-CombinedReport -RunId ('e' * 32) `
        -Profile ([ordered]@{ token = 'remote-lf4'; contract = Get-ProfileContract -ProfileToken remote-lf4 }) `
        -LogicalFps 2 -ModeClass Automatic -ScaleTarget '1.000'
    $zeroGeometryReport.decoder.telemetryBootstrapSuccesses = [UInt64]0
    $zeroGeometryReport.decoder.observedLocatorGeometry.samples = [UInt64]0
    foreach ($name in @('lastOriginX', 'lastOriginY', 'lastScaleX', 'lastScaleY', 'lastMarkerResidualPixels',
        'minimumOriginX', 'maximumOriginX', 'minimumOriginY', 'maximumOriginY', 'minimumScaleX', 'maximumScaleX',
        'minimumScaleY', 'maximumScaleY', 'minimumMarkerResidualPixels', 'maximumMarkerResidualPixels',
        'maximumScaleAnisotropy'))
    {
        $zeroGeometryReport.decoder.observedLocatorGeometry[$name] = $null
    }
    $zeroGeometry = Get-PBRemoteVisualObservedLocatorGeometry -DecoderReport $zeroGeometryReport.decoder
    $zeroGeometryRejected = $false
    try
    {
        Assert-PBRemoteVisualObservedLocatorTarget -Geometry $zeroGeometry -ScaleTarget '1.000' `
            -ProfileToken remote-lf4 -Context 'Fixture success'
    }
    catch { $zeroGeometryRejected = $_.Exception.Message -like '*lacks accepted Locator geometry*' }
    if (-not $zeroGeometryRejected)
    {
        throw 'Step 21 successful geometry contract accepted zero Locator samples'
    }

    $anisotropicGeometryReport = New-CombinedReport -RunId ('d' * 32) `
        -Profile ([ordered]@{ token = 'remote-lf4'; contract = Get-ProfileContract -ProfileToken remote-lf4 }) `
        -LogicalFps 2 -ModeClass Automatic -ScaleTarget '1.000'
    $anisotropicGeometryReport.decoder.observedLocatorGeometry.maximumScaleAnisotropy = 0.02
    $anisotropicGeometry = Get-PBRemoteVisualObservedLocatorGeometry -DecoderReport $anisotropicGeometryReport.decoder
    $anisotropicGeometryRejected = $false
    try
    {
        Assert-PBRemoteVisualObservedLocatorTarget -Geometry $anisotropicGeometry -ScaleTarget '1.000' `
            -ProfileToken remote-lf4 -Context 'Fixture success'
    }
    catch { $anisotropicGeometryRejected = $_.Exception.Message -like '*anisotropy tolerance*' }
    if (-not $anisotropicGeometryRejected)
    {
        throw 'Step 21 successful geometry contract ignored maximum observed scale anisotropy'
    }

    $geometrySampleMismatchReport = New-CombinedReport -RunId ('c' * 32) `
        -Profile ([ordered]@{ token = 'remote-lf4'; contract = Get-ProfileContract -ProfileToken remote-lf4 }) `
        -LogicalFps 2 -ModeClass Automatic -ScaleTarget '1.000'
    $geometrySampleMismatchReport.decoder.observedLocatorGeometry.samples = [UInt64]11
    $geometrySampleMismatchRejected = $false
    try { [void](Get-PBRemoteVisualObservedLocatorGeometry -DecoderReport $geometrySampleMismatchReport.decoder) }
    catch { $geometrySampleMismatchRejected = $_.Exception.Message -like '*successful Bootstrap telemetry*' }
    if (-not $geometrySampleMismatchRejected)
    {
        throw 'Step 21 geometry contract accepted a sample count that differs from Bootstrap successes'
    }

    $tupleRecord = Read-PBBoundedJson -Path (Join-Path $directories[0] 'matrix-run-record.json') -MaximumBytes 4MB
    $tupleRecord.matrix.backendCoverageRole = 'RepresentativeRecheck'
    $tupleRecordPath = Join-Path $runRoot 'matrix-record-wrong-coverage-role.json'
    Write-NewJson -Path $tupleRecordPath -Value $tupleRecord
    $tupleMismatchRejected = $false
    try
    {
        [void](Import-PBRemoteVisualMatrixRunRecord -Path $tupleRecordPath)
    }
    catch { $tupleMismatchRejected = $_.Exception.Message -like '*tuple differs*' }
    if (-not $tupleMismatchRejected)
    {
        throw 'Step 21 matrix record import did not bind its coverage role to the frozen plan'
    }

    $geometryRecord = Read-PBBoundedJson -Path (Join-Path $directories[0] 'matrix-run-record.json') -MaximumBytes 4MB
    $geometryRecord.matrix.observedLocatorGeometry.lastOriginX = 999.0
    $geometryRecordPath = Join-Path $runRoot 'matrix-record-tampered-observed-geometry.json'
    Write-NewJson -Path $geometryRecordPath -Value $geometryRecord
    $geometryRecordRejected = $false
    try
    {
        [void](Import-PBRemoteVisualMatrixRunRecord -Path $geometryRecordPath)
    }
    catch { $geometryRecordRejected = $_.Exception.Message -like '*observed Locator geometry differs*' }
    if (-not $geometryRecordRejected)
    {
        throw 'Step 21 matrix record import did not bind its observed geometry to the combined report'
    }

    $inspection = [ordered]@{
        schema = 'PixelBridge.RemoteVisualReplayInspection.1'
        frames = @(
            [ordered]@{
                ordinal = 0
                bootstrap = [ordered]@{ erasure = 'MarkersNotFound' }
                modulation = [ordered]@{
                    erasure = 'ScaleOutOfRange'; unreliablePrimary = 3; unreliableSecondary = $null
                    staleRegions = 1; freshnessTagMismatches = 1; freshnessTagErasures = 0; erasedDataMetrics = 2
                }
                transport = [ordered]@{ fecFailures = 1; crcFailures = 0; identityFailures = 0 }
            })
    }
    $encoderReport = [ordered]@{ submittedFrames = 10 }
    $decoderReport = [ordered]@{
        role = 'Decoder'; state = 'Stopped'; statusMessage = 'stopped after no progress'; errorDetail = ''
        duplicateFrameSequences = 1; reorderedFrameSequences = 0; frameSequenceGapEvents = 1; skippedFrameSequences = 0
        remoteMetricTelemetry = [ordered]@{ staleRegions = 1; freshnessTagMismatches = 1; freshnessTagErasures = 0; unreliableSymbols = 2; rejectedFrames = 1 }
        fecFailures = 1; crcFailures = 0; identityFailures = 0; captureDroppedFrames = 1; captureAcquireTimeouts = 0
        captureReadbackDropEvents = 0; staleResultDrops = 0; captureStall = [ordered]@{ count = 1 }
        visualStall = [ordered]@{ count = 1 }; captureArrivedFrames = 0
    }
    $failureSupport = Get-PBRemoteVisualFailureSupport -Inspection $inspection -EncoderReport $encoderReport `
        -LiveDecoderReport $decoderReport -OfflineDecoderReport $decoderReport
    foreach ($category in @('geometry', 'signal', 'temporal', 'metric', 'scheduler'))
    {
        if ($failureSupport[$category].supported -isnot [bool] -or -not [bool]$failureSupport[$category].supported)
        {
            throw "Step 21 failure classifier fixture lacks support for $category"
        }
    }

    $tamperedRecordPath = Join-Path $directories[0] 'matrix-run-record.json'
    Add-Content -LiteralPath $tamperedRecordPath -Value ' ' -NoNewline
    $tamperOutput = Join-Path $runRoot 'matrix-tampered-record'
    $tamperRejected = $false
    try
    {
        & $matrixTool -MatrixSpecPath $matrixSpecPath -ExpectedMatrixSpecSha256 $matrixSpecIdentity.sha256 `
            -RunEvidenceDirectory @($directories[0]) -OutputDirectory $tamperOutput | Out-Null
    }
    catch { $tamperRejected = $_.Exception.Message -like '*identity mismatch*' -or $_.Exception.Message -like '*SHA-256*' }
    if (-not $tamperRejected)
    {
        throw 'Step 21 matrix did not reject a sealed run-record mutation'
    }

    Write-Output 'PBRemoteVisual Step 21 matrix-evidence contracts: PASS'
}
finally
{
    $resolvedRunRoot = [System.IO.Path]::GetFullPath($runRoot)
    $workPrefix = $resolvedWorkRoot.TrimEnd('\') + '\'
    if ($resolvedRunRoot.StartsWith($workPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedRunRoot -PathType Container))
    {
        [System.IO.Directory]::Delete($resolvedRunRoot, $true)
    }
}

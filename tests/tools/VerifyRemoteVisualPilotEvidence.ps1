[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolsRoot,

    [Parameter(Mandatory = $true)]
    [string]$WorkRoot,

    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Tool
{
    param(
        [Parameter(Mandatory = $true)][string]$Script,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $output = @(& $script:PowerShell -NoProfile -NonInteractive -File $Script @Arguments 2>&1)
    return [ordered]@{ exitCode = $LASTEXITCODE; output = ($output | Out-String) }
}

function Require-Success
{
    param([object]$Result, [string]$Name)
    if ($Result.exitCode -ne 0)
    {
        throw "$Name unexpectedly failed with exit $($Result.exitCode): $($Result.output)"
    }
}

function Require-Failure
{
    param([object]$Result, [string]$Name, [string]$Pattern)
    if ($Result.exitCode -eq 0 -or $Result.output -notmatch $Pattern)
    {
        throw "$Name did not fail with expected diagnostic '$Pattern': $($Result.output)"
    }
}

function Write-NewJson
{
    param([string]$Path, [object]$Value)
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
        try { $writer.Write(($Value | ConvertTo-Json -Depth 30)); $writer.Flush(); $stream.Flush($true) }
        finally { $writer.Dispose() }
    }
    finally { $stream.Dispose() }
}

function New-Identity
{
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = [System.IO.Path]::GetFullPath($Path)
        size = [UInt64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$resolvedToolsRoot = [System.IO.Path]::GetFullPath($ToolsRoot)
$resolvedWorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
$resolvedPython = [System.IO.Path]::GetFullPath($PythonExecutable)
if (-not (Test-Path -LiteralPath $resolvedToolsRoot -PathType Container) -or
    -not (Test-Path -LiteralPath $resolvedPython -PathType Leaf))
{
    throw 'Pilot evidence tool root or Python executable does not exist'
}
if (-not (Test-Path -LiteralPath $resolvedWorkRoot -PathType Container))
{
    [void](New-Item -ItemType Directory -Path $resolvedWorkRoot)
}
$script:PowerShell = (Get-Command pwsh -ErrorAction Stop).Source
$scripts = @(
    'PBRemoteVisualPilotCommon.psm1',
    'Capture-PBRemoteVisualExperimentMonitor.ps1',
    'New-PBRemoteVisualUiEvidence.ps1',
    'Test-PBRemoteVisualUiEvidence.ps1',
    'New-PBRemoteVisualPilotPlan.ps1',
    'Invoke-PBRemoteVisualPilotEncoder.ps1',
    'Invoke-PBRemoteVisualPilotDecoder.ps1',
    'Test-PBRemoteVisualPilotEvidence.ps1'
)
foreach ($scriptName in $scripts)
{
    $scriptPath = Join-Path $resolvedToolsRoot $scriptName
    if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf))
    {
        throw "Required Step 20 script is missing: $scriptPath"
    }
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0)
    {
        throw "PowerShell parser rejected $scriptName`: $($parseErrors[0].Message)"
    }
    $scriptText = Get-Content -LiteralPath $scriptPath -Raw -Encoding UTF8
    if ($scriptText -notmatch '\A#Requires -Version 7\.0(?:\r?\n)')
    {
        throw "$scriptName does not fail closed with the frozen PowerShell 7 runtime prerequisite"
    }
}
$legacyPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $legacyPowerShell -PathType Leaf))
{
    throw 'Windows PowerShell 5.1 executable is unavailable for the Step 20 host-version negative test'
}
$legacyOutput = @(& $legacyPowerShell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
    -File (Join-Path $resolvedToolsRoot 'Invoke-PBRemoteVisualPilotEncoder.ps1') 2>&1)
$legacyExitCode = $LASTEXITCODE
if ($legacyExitCode -eq 0 -or ($legacyOutput | Out-String) -notmatch 'ScriptRequiresUnmatchedPSVersion')
{
    throw 'Windows PowerShell 5.1 did not fail closed before executing a Step 20 endpoint wrapper'
}
$persistentPathScripts = @(
    'Invoke-PBRemoteVisualPilotEncoder.ps1',
    'Invoke-PBRemoteVisualPilotDecoder.ps1',
    'Test-PBRemoteVisualPilotEvidence.ps1'
)
foreach ($scriptName in $persistentPathScripts)
{
    $scriptText = Get-Content -LiteralPath (Join-Path $resolvedToolsRoot $scriptName) -Raw -Encoding UTF8
    if ($scriptText -match '(?m)^\s*Move-Item\b')
    {
        throw "$scriptName moves an evidence directory after runtime reports persist absolute artifact paths"
    }
}
$encoderWrapperText = Get-Content -LiteralPath (Join-Path $resolvedToolsRoot 'Invoke-PBRemoteVisualPilotEncoder.ps1') -Raw -Encoding UTF8
$decoderWrapperText = Get-Content -LiteralPath (Join-Path $resolvedToolsRoot 'Invoke-PBRemoteVisualPilotDecoder.ps1') -Raw -Encoding UTF8
$finalVerifierText = Get-Content -LiteralPath (Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualPilotEvidence.ps1') -Raw -Encoding UTF8
if ($encoderWrapperText -notmatch "evidenceReadyUnixMilliseconds" -or
    $decoderWrapperText -notmatch "evidenceReadyUnixMilliseconds" -or
    $finalVerifierText -notmatch '-DecoderEvidenceReadyUnixMilliseconds \(\[Int64\]\$liveEndpoint\.process\.evidenceReadyUnixMilliseconds\)')
{
    throw 'Step 20 wrappers/verifier do not share the explicit post-validation evidence-ready proof anchor'
}

$runRoot = Join-Path $resolvedWorkRoot ("run-" + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $runRoot)
try
{
    Add-Type -AssemblyName System.Drawing.Common
    $screenshotPath = Join-Path $runRoot 'experiment-monitor.png'
    $bitmap = [System.Drawing.Bitmap]::new(640, 360, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try
    {
        $graphics.Clear([System.Drawing.Color]::FromArgb(25, 45, 70))
        $font = [System.Drawing.Font]::new('Arial', 22)
        try
        {
            $graphics.DrawString('Provider: ContractFixture', $font, [System.Drawing.Brushes]::White, 24, 32)
            $graphics.DrawString('Mode: VisibleQualityPanel', $font, [System.Drawing.Brushes]::White, 24, 84)
        }
        finally { $font.Dispose() }
        $bitmap.Save($screenshotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally
    {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    $screenshotIdentity = New-Identity -Path $screenshotPath
    $captureRecordPath = Join-Path $runRoot 'capture-record.json'
    Write-NewJson -Path $captureRecordPath -Value ([ordered]@{
        schema = 'PixelBridge.ExperimentMonitorScreenshotCapture.1'
        capturedUtc = [DateTime]::UtcNow.ToString('o')
        collectionSemantics = 'Exact ExperimentMonitor physical pixels; no input, focus, window, or display mutation'
        captureScope = 'ExperimentMonitorOnly'
        containsProtectedMonitorPixels = $false
        protectedMonitorDeviceName = 'PROTECTED'
        experimentMonitorDeviceName = 'EXPERIMENT'
        physicalRect = [ordered]@{ left = 1000; top = 0; right = 1640; bottom = 360 }
        path = $screenshotIdentity.path
        size = $screenshotIdentity.size
        sha256 = $screenshotIdentity.sha256
    })

    $metadataPath = Join-Path $runRoot 'metadata.json'
    $metadataResult = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'New-PBRemoteVisualRunPreset.ps1') -Arguments @(
        '-OutputPath', $metadataPath,
        '-RemoteProvider', 'ContractFixture',
        '-RemoteMode', 'VisibleQualityPanel',
        '-RemoteUiProvenance', 'RemoteUiVisible',
        '-ProtectedMonitorIdentity', 'PROTECTED',
        '-ExperimentMonitorIdentity', 'EXPERIMENT')
    Require-Success -Result $metadataResult -Name 'metadata fixture creation'

    $uiEvidencePath = Join-Path $runRoot 'ui-evidence.json'
    $uiResult = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'New-PBRemoteVisualUiEvidence.ps1') -Arguments @(
        '-MetadataPath', $metadataPath,
        '-ScreenshotPath', $screenshotPath,
        '-CaptureRecordPath', $captureRecordPath,
        '-OutputPath', $uiEvidencePath,
        '-PythonPath', $resolvedPython,
        '-VisibleProvider', 'ContractFixture',
        '-VisibleMode', 'VisibleQualityPanel')
    Require-Success -Result $uiResult -Name 'UI evidence creation'
    $uiVerificationPath = Join-Path $runRoot 'ui-verification.json'
    $uiVerify = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualUiEvidence.ps1') -Arguments @(
        '-UiEvidencePath', $uiEvidencePath,
        '-MetadataPath', $metadataPath,
        '-ScreenshotPath', $screenshotPath,
        '-CaptureRecordPath', $captureRecordPath,
        '-PythonPath', $resolvedPython,
        '-OutputPath', $uiVerificationPath)
    Require-Success -Result $uiVerify -Name 'UI evidence verification'
    $uiVerification = Get-Content -LiteralPath $uiVerificationPath -Raw | ConvertFrom-Json
    if ($uiVerification.status -cne 'PASS' -or $uiVerification.containsProtectedMonitorPixels)
    {
        throw 'UI evidence verification did not preserve the protected-monitor boundary'
    }
    $duplicateUi = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'New-PBRemoteVisualUiEvidence.ps1') -Arguments @(
        '-MetadataPath', $metadataPath, '-ScreenshotPath', $screenshotPath, '-CaptureRecordPath', $captureRecordPath,
        '-OutputPath', $uiEvidencePath, '-PythonPath', $resolvedPython,
        '-VisibleProvider', 'ContractFixture', '-VisibleMode', 'VisibleQualityPanel')
    Require-Failure -Result $duplicateUi -Name 'UI evidence create-only guard' -Pattern 'Create-only RemoteVisual UI evidence'
    $claimMismatch = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'New-PBRemoteVisualUiEvidence.ps1') -Arguments @(
        '-MetadataPath', $metadataPath, '-ScreenshotPath', $screenshotPath, '-CaptureRecordPath', $captureRecordPath,
        '-OutputPath', (Join-Path $runRoot 'ui-mismatch.json'), '-PythonPath', $resolvedPython,
        '-VisibleProvider', 'DifferentProvider', '-VisibleMode', 'VisibleQualityPanel')
    Require-Failure -Result $claimMismatch -Name 'UI visible-claim conflict guard' -Pattern 'Visible provider'
    $fpsMetadataPath = Join-Path $runRoot 'metadata-with-visible-fps.json'
    $fpsMetadataResult = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'New-PBRemoteVisualRunPreset.ps1') -Arguments @(
        '-OutputPath', $fpsMetadataPath,
        '-RemoteProvider', 'ContractFixture',
        '-RemoteMode', 'VisibleQualityPanel',
        '-TargetFps', '5',
        '-RemoteUiProvenance', 'RemoteUiVisible',
        '-ProtectedMonitorIdentity', 'PROTECTED',
        '-ExperimentMonitorIdentity', 'EXPERIMENT')
    Require-Success -Result $fpsMetadataResult -Name 'metadata with visible FPS fixture creation'
    $hiddenFps = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'New-PBRemoteVisualUiEvidence.ps1') -Arguments @(
        '-MetadataPath', $fpsMetadataPath, '-ScreenshotPath', $screenshotPath, '-CaptureRecordPath', $captureRecordPath,
        '-OutputPath', (Join-Path $runRoot 'ui-hidden-fps.json'), '-PythonPath', $resolvedPython,
        '-VisibleProvider', 'ContractFixture', '-VisibleMode', 'VisibleQualityPanel')
    Require-Failure -Result $hiddenFps -Name 'UI hidden target-FPS rejection' -Pattern 'Visible target FPS'
    $duplicateVerification = Invoke-Tool -Script (Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualUiEvidence.ps1') -Arguments @(
        '-UiEvidencePath', $uiEvidencePath, '-MetadataPath', $metadataPath, '-ScreenshotPath', $screenshotPath,
        '-CaptureRecordPath', $captureRecordPath, '-PythonPath', $resolvedPython, '-OutputPath', $uiVerificationPath)
    Require-Failure -Result $duplicateVerification -Name 'UI verification create-only guard' -Pattern 'Create-only UI evidence verification'

    $modulePath = Join-Path $resolvedToolsRoot 'PBRemoteVisualPilotCommon.psm1'
    Import-Module -Name $modulePath -Force -ErrorAction Stop
    $strictJson = ConvertFrom-PBStrictJsonText -Text '{"case":"sensitive","Case":"distinct"}' -Name 'strict JSON positive fixture'
    if ($strictJson.Count -ne 2 -or [string]$strictJson.case -cne 'sensitive' -or [string]$strictJson.Case -cne 'distinct')
    {
        throw 'Strict JSON positive fixture did not preserve case-sensitive property identity'
    }
    $duplicateJsonRejected = $false
    try { [void](ConvertFrom-PBStrictJsonText -Text '{"runId":"first","runId":"second"}' -Name 'duplicate fixture') }
    catch { $duplicateJsonRejected = $_.Exception.Message -like '*duplicate property*' }
    if (-not $duplicateJsonRejected)
    {
        throw 'Strict JSON reader did not reject an exact duplicate property'
    }
    $proofWindow = Get-PBPostDecoderBroadcastProofWindow -DecoderEvidenceReadyUnixMilliseconds 100000 `
        -DecoderClockOffsetMilliseconds -250 -ProofSeconds 7
    if ([Int64]$proofWindow.adjustedDecoderEvidenceReadyUnixMilliseconds -ne 99750 -or
        [Int64]$proofWindow.requiredProofMilliseconds -ne 7000 -or
        [Int64]$proofWindow.thresholdUnixMilliseconds -ne 106750)
    {
        throw 'Post-Decoder broadcast proof-window calculation is inconsistent'
    }
    $overflowRejected = $false
    try { [void](Get-PBPostDecoderBroadcastProofWindow -DecoderEvidenceReadyUnixMilliseconds ([Int64]::MaxValue) `
        -DecoderClockOffsetMilliseconds 0 -ProofSeconds 1) }
    catch { $overflowRejected = $_.Exception.Message -like '*overflows*' }
    if (-not $overflowRejected)
    {
        throw 'Post-Decoder broadcast proof-window overflow was not rejected'
    }
    $identity = [ordered]@{ path = $metadataPath; size = 1; sha256 = '0' * 64 }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json -AsHashtable -Depth 50
    $plan = [ordered]@{
        schema = 'PixelBridge.RemoteVisualPilotPlan.1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        runId = [string]$metadata.runId
        profileToken = 'remote-lf4'
        profileName = 'PB-RemoteVisual-LF4-X1 (Experimental)'
        visualProfileId = [UInt64]0x504252564C463431
        visualLayoutVersion = 7
        codedDataBytesPerFrame = 8100
        codewordsPerFrame = 4
        logicalFps = 5
        geometryMode = 'Strict1To1'
        deployment = [ordered]@{
            runId = [string]$metadata.runId
            manifest = $identity
            packageManifest = $identity
            sourceManifest = $identity
            remoteMetadata = $identity
            encoderEnvironment = $identity
            decoderEnvironment = $identity
        }
        applications = [ordered]@{
            encoder = [ordered]@{ role = 'Encoder'; relativeExecutablePath = 'Encoder/PixelBridgeEncoder.exe'; size = 1; sha256 = '1' * 64; versionOutput = 'fixture' }
            decoder = [ordered]@{ role = 'Decoder'; relativeExecutablePath = 'Decoder/PixelBridgeDecoder.exe'; size = 1; sha256 = '2' * 64; versionOutput = 'fixture' }
        }
        source = [ordered]@{ sourceSetId = '0' * 32; relativePath = 'random-1MiB.bin'; size = 1MB; sha256 = '3' * 64; pixelBridgeSegmentCompression = 'RAW/OFF' }
        remoteUi = [ordered]@{ runId = [string]$metadata.runId; evidence = $identity; screenshot = $identity; captureRecord = $identity }
        monitorSafety = [ordered]@{
            encoder = [ordered]@{ protectedMonitorDeviceName = 'P'; experimentMonitorDeviceName = 'E'; dataWindowPhysicalRect = [ordered]@{ left = 2000; top = 100; right = 3920; bottom = 1180 } }
            decoder = [ordered]@{ protectedMonitorDeviceName = 'P'; experimentMonitorDeviceName = 'E'; roiPhysicalRect = [ordered]@{ left = 2000; top = 100; right = 3920; bottom = 1180 } }
        }
        remoteGeometry = [ordered]@{ estimatedScaleX = 1.0; estimatedScaleY = 1.0 }
        policy = [ordered]@{
            captureBackend = 'wgc'; compression = 'off'; encoderHardMaximumSeconds = 180; decoderTimeoutSeconds = 120
            noProgressSeconds = 60; offlineReplayTimeoutSeconds = 600; offlineNoProgressSeconds = 120
            decoderWarmupMaximumSeconds = 30; controlRepetitions = 12; postPublishReplayTailSeconds = 2
            postDecoderBroadcastProofSeconds = 7; manualStopRequired = $true; decoderStartsBeforeEncoder = $true
            noFileClipboardOrIpcSideChannel = $true
            replay = [ordered]@{ maximumCaptureFramesPerSecond = 10; maximumFrames = 1250; maximumMiB = 16384; reservedNonFrameMiB = 256; worstCaseSampledFrames = 1222; worstCaseBytesIncludingReserve = 10636435456 }
            clock = [ordered]@{ decoderOffsetMilliseconds = 0; uncertaintyMilliseconds = 5000 }
        }
    }
    $planPath = Join-Path $runRoot 'plan.json'
    Write-NewJson -Path $planPath -Value $plan
    $frozenPlan = Import-PBRemoteVisualPilotPlan -Path $planPath
    if ($frozenPlan.value.logicalFps -ne 5 -or $frozenPlan.value.geometryMode -cne 'Strict1To1')
    {
        throw 'Frozen Step 20 plan fixture did not validate'
    }
    $duplicatePlanPath = Join-Path $runRoot 'plan-duplicate-key.json'
    $planText = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8
    $schemaToken = '"schema": "PixelBridge.RemoteVisualPilotPlan.1"'
    $duplicatePlanText = $planText.Replace($schemaToken, "$schemaToken,`n  $schemaToken")
    if ($duplicatePlanText -ceq $planText)
    {
        throw 'Pilot duplicate-key fixture injection failed'
    }
    [System.IO.File]::WriteAllText($duplicatePlanPath, $duplicatePlanText, [System.Text.UTF8Encoding]::new($false))
    $duplicatePlanRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $duplicatePlanPath) }
    catch { $duplicatePlanRejected = $_.Exception.Message -like '*duplicate property*' }
    if (-not $duplicatePlanRejected)
    {
        throw 'Pilot plan reader did not reject an exact duplicate property'
    }
    $wrongExpectedHashRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $planPath -ExpectedSha256 ('f' * 64)) }
    catch { $wrongExpectedHashRejected = $_.Exception.Message -like '*SHA-256 differs*' }
    if (-not $wrongExpectedHashRejected)
    {
        throw 'Pilot plan expected-hash mismatch was not rejected'
    }
    $caseTamperedPlan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $caseTamperedPlan.schema = 'pixelbridge.remotevisualpilotplan.1'
    $caseTamperedPlanPath = Join-Path $runRoot 'plan-schema-case-tampered.json'
    Write-NewJson -Path $caseTamperedPlanPath -Value $caseTamperedPlan
    $caseTamperedPlanRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $caseTamperedPlanPath) }
    catch { $caseTamperedPlanRejected = $_.Exception.Message -like '*schema or RunId is invalid*' }
    if (-not $caseTamperedPlanRejected)
    {
        throw 'Pilot plan importer accepted a case-tampered schema identity'
    }
    $tamperedPlan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $tamperedPlan.policy.replay.worstCaseBytesIncludingReserve++
    $tamperedPlanPath = Join-Path $runRoot 'plan-tampered.json'
    Write-NewJson -Path $tamperedPlanPath -Value $tamperedPlan
    $tamperedPlanRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $tamperedPlanPath) }
    catch { $tamperedPlanRejected = $_.Exception.Message -like '*replay byte budget*' }
    if (-not $tamperedPlanRejected)
    {
        throw 'Pilot plan Replay resource tamper was not rejected'
    }
    $scaledPlan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $scaledPlan.geometryMode = 'LocatorScaled'
    $scaledPlanPath = Join-Path $runRoot 'plan-invalid-scaled.json'
    Write-NewJson -Path $scaledPlanPath -Value $scaledPlan
    $scaledPlanRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $scaledPlanPath) }
    catch { $scaledPlanRejected = $_.Exception.Message -like '*must exercise a non-1:1*' }
    if (-not $scaledPlanRejected)
    {
        throw 'Pilot plan fake scaled geometry was not rejected'
    }

    $step21Plan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21Plan.schema = 'PixelBridge.RemoteVisualPilotPlan.3'
    $step21Plan.profileToken = 'direct'
    $step21Plan.profileName = 'Direct-Level 2x2 (Experimental)'
    $step21Plan.visualProfileId = [UInt64]::Parse('EBB15DCE41AB436E', [Globalization.NumberStyles]::HexNumber)
    $step21Plan.visualLayoutVersion = 3
    $step21Plan.codedDataBytesPerFrame = 86688
    $step21Plan.codewordsPerFrame = 42
    $step21Plan.policy.captureBackend = 'dxgi'
    $step21Plan.policy.replay.maximumFrames = [UInt32]$step21Plan.policy.replay.worstCaseSampledFrames
    $step21Plan.policy.replay.worstCaseBytesIncludingReserve =
        [UInt64]1920 * [UInt64]1080 * 4 * [UInt64]$step21Plan.policy.replay.maximumFrames + 256MB
    $step21Plan.remoteUi.provenance = 'RemoteUiVisible'
    $step21Plan.remoteUi.visibleFields = @('remoteProvider', 'remoteMode')
    $step21Plan.remoteUi.visibleClaims = [ordered]@{ remoteProvider = 'ContractFixture'; remoteMode = 'VisibleQualityPanel' }
    $step21Plan.remoteGeometry = [ordered]@{
        captureRoiWidth = [UInt32]1920
        captureRoiHeight = [UInt32]1080
        captureRoiScaleX = 1.0
        captureRoiScaleY = 1.0
        expectedLocatorScaleTarget = '1.000'
        scaleAuthority = 'AcceptedBootstrapLocatorPixels'
        axisAligned = $true
        cropStatus = 'NoneExpected'
        locatorRemainsAuthoritative = $true
    }
    $step21Plan.matrix = [ordered]@{
        modeClass = 'QualityPriority'
        visibleRemoteMode = 'VisibleQualityPanel'
        scaleTarget = '1.000'
        profileComparisonRole = 'Baseline'
        backendCoverageRole = 'RepresentativeRecheck'
        runIsolation = 'IndependentRunIdAndArtifacts; metrics from different runs must not be merged'
    }
    $protectedRect = [ordered]@{ left = 0; top = 0; right = 1920; bottom = 1200 }
    $experimentRect = [ordered]@{ left = 1920; top = 0; right = 4480; bottom = 1440 }
    $protectedContract = [ordered]@{
        deviceName = 'P'; physicalRect = $protectedRect; dpiX = 96; dpiY = 96; refreshRate = 60
        rotation = 'Identity'; adapterLuid = [ordered]@{ high = 0; low = 1 }; primary = $true
    }
    $experimentContract = [ordered]@{
        deviceName = 'E'; physicalRect = $experimentRect; dpiX = 96; dpiY = 96; refreshRate = 60
        rotation = 'Identity'; adapterLuid = [ordered]@{ high = 0; low = 1 }; primary = $false
    }
    foreach ($endpoint in @($step21Plan.monitorSafety.encoder, $step21Plan.monitorSafety.decoder))
    {
        $endpoint.protectedMonitorPhysicalRect = $protectedRect
        $endpoint.experimentMonitorPhysicalRect = $experimentRect
        $endpoint.protectedMonitorContract = $protectedContract
        $endpoint.experimentMonitorContract = $experimentContract
    }
    $step21Plan.monitorSafety.encoder.runtimeEnforcement =
        'PlanAndEndpointEnvironmentContainment; current Direct/Shape Encoder CLI does not accept monitor-safety arguments'
    $step21Plan.monitorSafety.decoder.runtimeEnforcement = 'ProductionRuntimePreflightAndPeriodicRevalidation'
    $step21PlanPath = Join-Path $runRoot 'plan-step21-direct-dxgi.json'
    Write-NewJson -Path $step21PlanPath -Value $step21Plan
    $frozenStep21Plan = Import-PBRemoteVisualPilotPlan -Path $step21PlanPath
    if ([string]$frozenStep21Plan.value.profileToken -cne 'direct' -or
        [string]$frozenStep21Plan.value.policy.captureBackend -cne 'dxgi' -or
        [string]$frozenStep21Plan.value.matrix.modeClass -cne 'QualityPriority')
    {
        throw 'Frozen Step 21 Direct/DXGI plan fixture did not validate'
    }

    $step21ScaledDirect = Get-Content -LiteralPath $step21PlanPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21ScaledDirect.geometryMode = 'LocatorScaled'
    $step21ScaledDirectPath = Join-Path $runRoot 'plan-step21-invalid-direct-scaled.json'
    Write-NewJson -Path $step21ScaledDirectPath -Value $step21ScaledDirect
    $step21ScaledDirectRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $step21ScaledDirectPath) }
    catch { $step21ScaledDirectRejected = $_.Exception.Message -like '*target 1.000*' }
    if (-not $step21ScaledDirectRejected)
    {
        throw 'Step 21 Direct plan did not reject scaled geometry'
    }

    $step21ModeTamper = Get-Content -LiteralPath $step21PlanPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21ModeTamper.matrix.visibleRemoteMode = 'DifferentVisibleMode'
    $step21ModeTamperPath = Join-Path $runRoot 'plan-step21-mode-tamper.json'
    Write-NewJson -Path $step21ModeTamperPath -Value $step21ModeTamper
    $step21ModeTamperRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $step21ModeTamperPath) }
    catch { $step21ModeTamperRejected = $_.Exception.Message -like '*visible UI evidence*' }
    if (-not $step21ModeTamperRejected)
    {
        throw 'Step 21 plan did not reject a mode/UI-evidence mismatch'
    }

    $step21LegacyScaleLeak = Get-Content -LiteralPath $step21PlanPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21LegacyScaleLeak.remoteGeometry['estimatedScaleX'] = 1.0
    $step21LegacyScaleLeakPath = Join-Path $runRoot 'plan-step21-legacy-scale-leak.json'
    Write-NewJson -Path $step21LegacyScaleLeakPath -Value $step21LegacyScaleLeak
    $step21LegacyScaleLeakRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $step21LegacyScaleLeakPath) }
    catch { $step21LegacyScaleLeakRejected = $_.Exception.Message -like '*exact required key set*' }
    if (-not $step21LegacyScaleLeakRejected)
    {
        throw 'Step 21 Plan.3 accepted a legacy ROI-derived estimated scale field'
    }

    $step21FractionalRoi = Get-Content -LiteralPath $step21PlanPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21FractionalRoi.remoteGeometry.captureRoiWidth = 1920.25
    $step21FractionalRoiPath = Join-Path $runRoot 'plan-step21-fractional-roi.json'
    Write-NewJson -Path $step21FractionalRoiPath -Value $step21FractionalRoi
    $step21FractionalRoiRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $step21FractionalRoiPath) }
    catch { $step21FractionalRoiRejected = $_.Exception.Message -like '*UInt32 integer*' }
    if (-not $step21FractionalRoiRejected)
    {
        throw 'Step 21 Plan.3 silently rounded a fractional capture ROI dimension'
    }

    $step21StringScale = Get-Content -LiteralPath $step21PlanPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21StringScale.remoteGeometry.captureRoiScaleX = 'NaN'
    $step21StringScalePath = Join-Path $runRoot 'plan-step21-string-scale.json'
    Write-NewJson -Path $step21StringScalePath -Value $step21StringScale
    $step21StringScaleRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $step21StringScalePath) }
    catch { $step21StringScaleRejected = $_.Exception.Message -like '*finite JSON number*' }
    if (-not $step21StringScaleRejected)
    {
        throw 'Step 21 Plan.3 accepted a string-valued non-finite capture ROI scale'
    }

    $step21MatrixLegacyScaleLeak = Get-Content -LiteralPath $step21PlanPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $step21MatrixLegacyScaleLeak.matrix['estimatedScaleX'] = 1.0
    $step21MatrixLegacyScaleLeakPath = Join-Path $runRoot 'plan-step21-matrix-legacy-scale-leak.json'
    Write-NewJson -Path $step21MatrixLegacyScaleLeakPath -Value $step21MatrixLegacyScaleLeak
    $step21MatrixLegacyScaleLeakRejected = $false
    try { [void](Import-PBRemoteVisualPilotPlan -Path $step21MatrixLegacyScaleLeakPath) }
    catch { $step21MatrixLegacyScaleLeakRejected = $_.Exception.Message -like '*exact required key set*' }
    if (-not $step21MatrixLegacyScaleLeakRejected)
    {
        throw 'Step 21 Plan.3 matrix tuple accepted a legacy ROI-derived estimated scale field'
    }

    $monitorProbePath = Join-Path $runRoot 'mock-monitor-probe.ps1'
    $monitorCatalogJson = [ordered]@{
        schema = 'PixelBridge.MonitorCatalog.1'
        monitorCount = 2
        monitors = @(
            [ordered]@{
                deviceName = 'P'; physicalRect = $protectedRect; dpiX = 96; dpiY = 96; refreshRate = 60
                rotation = 'Identity'; adapterLuid = [ordered]@{ high = 0; low = 1 }; primary = $true
            },
            [ordered]@{
                deviceName = 'E'; physicalRect = $experimentRect; dpiX = 96; dpiY = 96; refreshRate = 60
                rotation = 'Identity'; adapterLuid = [ordered]@{ high = 0; low = 1 }; primary = $false
            })
    } | ConvertTo-Json -Depth 10 -Compress
    $monitorProbeScript = "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(`$false)`n" +
        "Write-Output '$monitorCatalogJson'`nexit 0`n"
    [System.IO.File]::WriteAllText($monitorProbePath, $monitorProbeScript, [System.Text.UTF8Encoding]::new($false))
    $monitorPreflightPath = Join-Path $runRoot 'monitor-preflight.json'
    $monitorPreflightIdentity = New-PBRemoteVisualMonitorPreflight -DecoderPath $monitorProbePath `
        -Safety $step21Plan.monitorSafety.encoder -TargetRect $step21Plan.monitorSafety.encoder.dataWindowPhysicalRect `
        -EndpointRole Encoder -OutputPath $monitorPreflightPath
    $monitorPreflight = Read-PBBoundedJson -Path $monitorPreflightPath -MaximumBytes 2MB
    if ([string]$monitorPreflight.schema -cne 'PixelBridge.RemoteVisualMonitorPreflight.1' -or
        [string]$monitorPreflight.status -cne 'PASS' -or [UInt64]$monitorPreflightIdentity.size -eq 0)
    {
        throw 'Step 21 live monitor preflight fixture did not validate'
    }
    $driftedMonitorProbePath = Join-Path $runRoot 'mock-monitor-probe-drifted.ps1'
    $driftedMonitorCatalogJson = $monitorCatalogJson.Replace('"refreshRate":60', '"refreshRate":61')
    $driftedMonitorProbeScript = "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(`$false)`n" +
        "Write-Output '$driftedMonitorCatalogJson'`nexit 0`n"
    [System.IO.File]::WriteAllText($driftedMonitorProbePath, $driftedMonitorProbeScript, [System.Text.UTF8Encoding]::new($false))
    $monitorDriftRejected = $false
    try
    {
        [void](New-PBRemoteVisualMonitorPreflight -DecoderPath $driftedMonitorProbePath `
            -Safety $step21Plan.monitorSafety.encoder -TargetRect $step21Plan.monitorSafety.encoder.dataWindowPhysicalRect `
            -EndpointRole Encoder -OutputPath (Join-Path $runRoot 'monitor-preflight-drifted.json'))
    }
    catch { $monitorDriftRejected = $_.Exception.Message -like '*differs from the frozen*' }
    if (-not $monitorDriftRejected)
    {
        throw 'Step 21 live monitor preflight did not reject monitor identity drift'
    }

    Write-Output 'PBRemoteVisual pilot-evidence contracts: PASS'
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

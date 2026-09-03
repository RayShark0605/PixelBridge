[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolsRoot,

    [Parameter(Mandatory = $true)]
    [string]$WorkRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory
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
    $exitCode = $LASTEXITCODE
    return [ordered]@{
        exitCode = $exitCode
        output = ($output | Out-String)
    }
}

function Require-Success
{
    param(
        [Parameter(Mandatory = $true)][object]$Result,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Result.exitCode -ne 0)
    {
        throw "$Name unexpectedly failed with exit $($Result.exitCode): $($Result.output)"
    }
}

function Require-Failure
{
    param(
        [Parameter(Mandatory = $true)][object]$Result,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Pattern
    )
    if ($Result.exitCode -eq 0 -or $Result.output -notmatch $Pattern)
    {
        throw "$Name did not fail with the expected diagnostic '$Pattern': $($Result.output)"
    }
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

$resolvedToolsRoot = [System.IO.Path]::GetFullPath($ToolsRoot)
$resolvedWorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
$resolvedBuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
if (-not (Test-Path -LiteralPath $resolvedToolsRoot -PathType Container))
{
    throw "RemoteVisual evidence tool root does not exist: $resolvedToolsRoot"
}
if (-not (Test-Path -LiteralPath $resolvedBuildDirectory -PathType Container))
{
    throw "RemoteVisual build directory does not exist: $resolvedBuildDirectory"
}
if (-not (Test-Path -LiteralPath $resolvedWorkRoot -PathType Container))
{
    [void](New-Item -ItemType Directory -Path $resolvedWorkRoot)
}
$script:PowerShell = (Get-Command pwsh -ErrorAction Stop).Source
$scripts = @(
    'New-PBRemoteVisualPortablePackage.ps1',
    'Test-PBRemoteVisualPortablePackage.ps1',
    'New-PBRemoteVisualSourceSet.ps1',
    'Test-PBRemoteVisualSourceSet.ps1',
    'New-PBRemoteVisualStep21ComputerBKit.ps1',
    'Test-PBRemoteVisualStep21ComputerBKit.ps1',
    'PBRemoteVisualStep22Common.psm1',
    'New-PBRemoteVisualStep22Campaign.ps1',
    'Test-PBRemoteVisualStep22Campaign.ps1',
    'New-PBRemoteVisualRunPreset.ps1',
    'Get-PBRemoteVisualEnvironment.ps1',
    'New-PBRemoteVisualDeploymentManifest.ps1',
    'Test-PBRemoteVisualDeploymentManifest.ps1'
)
foreach ($scriptName in $scripts)
{
    $scriptPath = Join-Path $resolvedToolsRoot $scriptName
    if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf))
    {
        throw "Required Step 18 script is missing: $scriptPath"
    }
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0)
    {
        throw "PowerShell parser rejected $scriptName`: $($parseErrors[0].Message)"
    }
}

$runRoot = Join-Path $resolvedWorkRoot ("run-" + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $runRoot)
try
{
    $sourceCreator = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualSourceSet.ps1'
    $sourceVerifier = Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualSourceSet.ps1'
    $presetCreator = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualRunPreset.ps1'
    $sourceSetDirectory = Join-Path $runRoot 'source-set'
    $sourceSealPath = Join-Path $runRoot 'source-set.seal.json'
    $createResult = Invoke-Tool -Script $sourceCreator -Arguments @('-OutputDirectory', $sourceSetDirectory)
    Require-Success -Result $createResult -Name 'source-set creation'
    $sourceManifestPath = Join-Path $sourceSetDirectory 'source-manifest.json'
    $manifest = Get-Content -LiteralPath $sourceManifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -cne 'PixelBridge.RemoteVisualSourceSet.2' -or $manifest.files.Count -ne 3 -or
        $manifest.sourceSetId -cnotmatch '^[0-9a-f]{32}$' -or
        (Test-Path -LiteralPath (Join-Path $sourceSetDirectory 'zip-payload-4MiB.bin.partial')))
    {
        throw 'Generated source-set manifest/inventory is invalid or the ZIP intermediate leaked'
    }
    $manifestHash = (Get-FileHash -LiteralPath $sourceManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $verificationPath = Join-Path $runRoot 'source-verification.json'
    $verifyResult = Invoke-Tool -Script $sourceVerifier -Arguments @(
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSealPath', $sourceSealPath,
        '-ExpectedManifestSha256', $manifestHash,
        '-OutputPath', $verificationPath)
    Require-Success -Result $verifyResult -Name 'source-set verification'
    $verification = Get-Content -LiteralPath $verificationPath -Raw | ConvertFrom-Json
    if (-not $verification.verified -or $verification.sourceSetId -cne $manifest.sourceSetId)
    {
        throw 'Source-set verification artifact is not bound to the generated identity'
    }

    $duplicateVerification = Invoke-Tool -Script $sourceVerifier -Arguments @(
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSealPath', $sourceSealPath,
        '-OutputPath', $verificationPath)
    Require-Failure -Result $duplicateVerification -Name 'verification create-only guard' -Pattern 'Create-only verification output'
    $wrongManifest = Invoke-Tool -Script $sourceVerifier -Arguments @(
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSealPath', $sourceSealPath,
        '-ExpectedManifestSha256', ('0' * 64))
    Require-Failure -Result $wrongManifest -Name 'source manifest identity guard' -Pattern 'does not match the expected deployed identity'

    $tamperRoot = Join-Path $runRoot 'tamper'
    [void](New-Item -ItemType Directory -Path $tamperRoot)
    Copy-Item -LiteralPath $sourceSetDirectory -Destination (Join-Path $tamperRoot 'source-set') -Recurse
    Copy-Item -LiteralPath $sourceSealPath -Destination (Join-Path $tamperRoot 'source-set.seal.json')
    $tamperPath = Join-Path $tamperRoot 'source-set\random-1MiB.bin'
    $tamperStream = [System.IO.File]::Open($tamperPath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try
    {
        $original = $tamperStream.ReadByte()
        $tamperStream.Position = 0
        $tamperStream.WriteByte([byte]($original -bxor 0xff))
        $tamperStream.Flush($true)
    }
    finally
    {
        $tamperStream.Dispose()
    }
    $tamperResult = Invoke-Tool -Script $sourceVerifier -Arguments @(
        '-SourceSetDirectory', (Join-Path $tamperRoot 'source-set'),
        '-SourceSealPath', (Join-Path $tamperRoot 'source-set.seal.json'))
    Require-Failure -Result $tamperResult -Name 'source payload tamper guard' -Pattern 'file identity mismatch'

    $duplicateCreation = Invoke-Tool -Script $sourceCreator -Arguments @('-OutputDirectory', $sourceSetDirectory)
    Require-Failure -Result $duplicateCreation -Name 'source-set create-only guard' -Pattern 'Create-only source-set output already exists'
    if ((Get-FileHash -LiteralPath $sourceManifestPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifestHash -or
        (Test-Path -LiteralPath "$sourceSetDirectory.partial"))
    {
        throw 'Rejected duplicate source-set creation changed the original or left a partial directory'
    }

    $packageCreator = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualPortablePackage.ps1'
    $packageVerifier = Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualPortablePackage.ps1'
    $packageOutputRoot = Join-Path $runRoot 'portable-package'
    [void](New-Item -ItemType Directory -Path $packageOutputRoot)
    $packageCreation = Invoke-Tool -Script $packageCreator -Arguments @(
        '-Role', 'Both',
        '-Label', 'current-head',
        '-BuildDirectory', $resolvedBuildDirectory,
        '-OutputRoot', $packageOutputRoot,
        '-ExcludedSourcePath', 'docs/PHASE1_GATE_REPORT.md',
        '-CompactPackageName')
    Require-Success -Result $packageCreation -Name 'Both-role portable-package creation for Computer B kit'
    $packageCreateResult = $packageCreation.output | ConvertFrom-Json
    if ($packageCreateResult.packageDirectory -isnot [string] -or
        $packageCreateResult.manifestSha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw 'Portable-package creator did not return a valid Computer B kit input identity'
    }

    $campaignCreator = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualStep22Campaign.ps1'
    $campaignVerifier = Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualStep22Campaign.ps1'
    $campaignPath = Join-Path $runRoot 'step22-campaign.json'
    $packageManifestForCampaign = Get-Content -LiteralPath `
        (Join-Path ([string]$packageCreateResult.packageDirectory) 'package-manifest.json') -Raw | ConvertFrom-Json
    $campaignCommit = [string]$packageManifestForCampaign.buildIdentity.headCommit
    $campaignCreation = Invoke-Tool -Script $campaignCreator -Arguments @(
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-ExpectedPackageManifestSha256', [string]$packageCreateResult.manifestSha256,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath,
        '-ExpectedSourceManifestSha256', $manifestHash,
        '-ExpectedGitCommit', $campaignCommit,
        '-OutputPath', $campaignPath)
    Require-Success -Result $campaignCreation -Name 'Step 22 create-only campaign creation'
    $campaignCreationValue = $campaignCreation.output | ConvertFrom-Json
    $campaign = Get-Content -LiteralPath $campaignPath -Raw | ConvertFrom-Json
    if ($campaignCreationValue.status -cne 'READY_NOT_EXECUTED' -or $campaignCreationValue.runCount -ne 6 -or
        $campaignCreationValue.remoteVisualSmokePass -or $campaignCreationValue.certifiedRemoteVisualProfile -or
        $campaign.schema -cne 'PixelBridge.RemoteVisualStep22Campaign.1' -or
        @($campaign.runs).Count -ne 6 -or @($campaign.runs.runId | Sort-Object -Unique).Count -ne 6 -or
        @($campaign.runs | Where-Object { $_.sourceRelativePath -ceq 'random-1MiB.bin' }).Count -ne 3 -or
        @($campaign.runs | Where-Object { $_.sourceRelativePath -ceq 'random-8MiB.bin' }).Count -ne 2 -or
        @($campaign.runs | Where-Object { $_.sourceRelativePath -ceq 'random-payload-4MiB.zip' }).Count -ne 1)
    {
        throw 'Step 22 campaign creator inflated truth or produced an invalid 3/2/1 schedule'
    }
    $campaignHash = (Get-FileHash -LiteralPath $campaignPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $campaignVerificationPath = Join-Path $runRoot 'step22-campaign-verification.json'
    $campaignVerification = Invoke-Tool -Script $campaignVerifier -Arguments @(
        '-CampaignPath', $campaignPath,
        '-ExpectedCampaignSha256', $campaignHash,
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath,
        '-OutputPath', $campaignVerificationPath)
    Require-Success -Result $campaignVerification -Name 'Step 22 campaign verification'
    $campaignVerificationValue = $campaignVerification.output | ConvertFrom-Json
    if (-not $campaignVerificationValue.verified -or $campaignVerificationValue.status -cne 'READY_NOT_EXECUTED' -or
        $campaignVerificationValue.runCount -ne 6 -or $campaignVerificationValue.remoteVisualSmokePass -or
        $campaignVerificationValue.certifiedRemoteVisualProfile)
    {
        throw 'Step 22 campaign verifier inflated readiness into execution, smoke, or certification'
    }

    $duplicateCampaign = Invoke-Tool -Script $campaignCreator -Arguments @(
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-ExpectedPackageManifestSha256', [string]$packageCreateResult.manifestSha256,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath,
        '-ExpectedSourceManifestSha256', $manifestHash,
        '-ExpectedGitCommit', $campaignCommit,
        '-OutputPath', $campaignPath)
    Require-Failure -Result $duplicateCampaign -Name 'Step 22 campaign create-only guard' `
        -Pattern 'Create-only Step 22 campaign or partial already exists'
    $duplicateCampaignVerification = Invoke-Tool -Script $campaignVerifier -Arguments @(
        '-CampaignPath', $campaignPath,
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath,
        '-OutputPath', $campaignVerificationPath)
    Require-Failure -Result $duplicateCampaignVerification -Name 'Step 22 verification create-only guard' `
        -Pattern 'Create-only JSON output or partial already exists'

    $duplicateRunIdPath = Join-Path $runRoot 'step22-duplicate-run-id.json'
    $duplicateRunId = Get-Content -LiteralPath $campaignPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $duplicateRunId.runs[1].runId = [string]$duplicateRunId.runs[0].runId
    $duplicateRunId.runs[1].evidenceDirectoryName = [string]$duplicateRunId.runs[0].runId
    [System.IO.File]::WriteAllText($duplicateRunIdPath, ($duplicateRunId | ConvertTo-Json -Depth 100),
        [System.Text.UTF8Encoding]::new($false))
    $duplicateRunIdVerification = Invoke-Tool -Script $campaignVerifier -Arguments @(
        '-CampaignPath', $duplicateRunIdPath,
        '-ExpectedCampaignSha256', (Get-FileHash -LiteralPath $duplicateRunIdPath -Algorithm SHA256).Hash.ToLowerInvariant(),
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath)
    Require-Failure -Result $duplicateRunIdVerification -Name 'Step 22 duplicate RunId guard' `
        -Pattern 'run IDs must be unique'

    $stringIntegerPath = Join-Path $runRoot 'step22-string-integer.json'
    $stringInteger = Get-Content -LiteralPath $campaignPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $stringInteger.policy.decoderTimeoutSeconds = '3600'
    [System.IO.File]::WriteAllText($stringIntegerPath, ($stringInteger | ConvertTo-Json -Depth 100),
        [System.Text.UTF8Encoding]::new($false))
    $stringIntegerVerification = Invoke-Tool -Script $campaignVerifier -Arguments @(
        '-CampaignPath', $stringIntegerPath,
        '-ExpectedCampaignSha256', (Get-FileHash -LiteralPath $stringIntegerPath -Algorithm SHA256).Hash.ToLowerInvariant(),
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath)
    Require-Failure -Result $stringIntegerVerification -Name 'Step 22 JSON numeric-type guard' `
        -Pattern 'decoderTimeoutSeconds must be a non-negative UInt64 JSON integer'

    $wrongRunSourcePath = Join-Path $runRoot 'step22-wrong-run-source.json'
    $wrongRunSource = Get-Content -LiteralPath $campaignPath -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    $wrongRunSource.runs[0].sourceSha256 = 'f' * 64
    [System.IO.File]::WriteAllText($wrongRunSourcePath, ($wrongRunSource | ConvertTo-Json -Depth 100),
        [System.Text.UTF8Encoding]::new($false))
    $wrongRunSourceVerification = Invoke-Tool -Script $campaignVerifier -Arguments @(
        '-CampaignPath', $wrongRunSourcePath,
        '-ExpectedCampaignSha256', (Get-FileHash -LiteralPath $wrongRunSourcePath -Algorithm SHA256).Hash.ToLowerInvariant(),
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath)
    Require-Failure -Result $wrongRunSourceVerification -Name 'Step 22 run/source identity guard' `
        -Pattern 'run source identity differs from the sealed source set'

    $kitCreator = Join-Path $resolvedToolsRoot 'New-PBRemoteVisualStep21ComputerBKit.ps1'
    $kitVerifier = Join-Path $resolvedToolsRoot 'Test-PBRemoteVisualStep21ComputerBKit.ps1'
    $kitOutputRoot = Join-Path $runRoot 'computer-b-kit'
    [void](New-Item -ItemType Directory -Path $kitOutputRoot)
    $headCommit = (& git -C ([System.IO.Path]::GetFullPath((Join-Path $resolvedToolsRoot '..\..'))) rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $headCommit -cnotmatch '^[0-9a-f]{40}$')
    {
        throw 'Computer B kit test could not resolve repository HEAD'
    }
    $kitCreation = Invoke-Tool -Script $kitCreator -Arguments @(
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-PackageArchivePath', [string]$packageCreateResult.archivePath,
        '-ExpectedPackageManifestSha256', [string]$packageCreateResult.manifestSha256,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath,
        '-ExpectedSourceManifestSha256', $manifestHash,
        '-ExpectedHeadCommit', $headCommit,
        '-ComputerBProtectedMonitorDeviceName', '\\.\DISPLAY2',
        '-ComputerBExperimentMonitorDeviceName', '\\.\DISPLAY1',
        '-OutputRoot', $kitOutputRoot)
    Require-Success -Result $kitCreation -Name 'single-extraction Computer B kit creation'
    $kitCreateResult = $kitCreation.output | ConvertFrom-Json
    if ($kitCreateResult.status -cne 'PREDEPLOYMENT_ONLY' -or $kitCreateResult.executedCellCount -ne 0 -or
        $kitCreateResult.formalStep21Accepted -or $kitCreateResult.certifiedRemoteVisualProfile -or
        $kitCreateResult.manifestSha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $kitCreateResult.archiveSha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw 'Computer B kit creator inflated its truth boundary or returned an invalid identity'
    }
    $kitVerification = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-KitSealPath', [string]$kitCreateResult.sealPath,
        '-ArchivePath', [string]$kitCreateResult.archivePath,
        '-ExpectedManifestSha256', [string]$kitCreateResult.manifestSha256)
    Require-Success -Result $kitVerification -Name 'Computer B kit directory/archive verification'
    $kitVerificationValue = $kitVerification.output | ConvertFrom-Json
    if (-not $kitVerificationValue.verified -or $kitVerificationValue.status -cne 'PREDEPLOYMENT_ONLY' -or
        $kitVerificationValue.executedCellCount -ne 0)
    {
        throw 'Computer B kit verifier did not preserve the predeployment-only truth boundary'
    }
    $kitManifest = Get-Content -LiteralPath (Join-Path ([string]$kitCreateResult.kitDirectory) 'COMPUTER-B-KIT-MANIFEST.json') `
        -Raw | ConvertFrom-Json
    $packagedDecoderPath = Join-Path ([string]$kitCreateResult.kitDirectory) `
        (Join-Path ([string]$kitManifest.package.directoryName) 'Decoder\PixelBridgeDecoder.exe')
    $packagedCatalogText = @(& $packagedDecoderPath '--list-monitors' 2>&1) -join "`n"
    $packagedCatalogExitCode = $LASTEXITCODE
    if ($packagedCatalogExitCode -ne 0)
    {
        throw "Computer B kit packaged Decoder could not execute from its deployed path (length=$($packagedDecoderPath.Length)): $packagedCatalogText"
    }
    $packagedCatalog = $packagedCatalogText | ConvertFrom-Json
    if ([string]$packagedCatalog.schema -cne 'PixelBridge.MonitorCatalog.1' -or @($packagedCatalog.monitors).Count -eq 0)
    {
        throw 'Computer B kit packaged Decoder did not return a valid local monitor catalog'
    }

    $packageManifestPath = Join-Path ([string]$packageCreateResult.packageDirectory) 'package-manifest.json'
    $originalPackageManifestText = [System.IO.File]::ReadAllText($packageManifestPath)
    try
    {
        $runtimeIdentityTamper = $originalPackageManifestText | ConvertFrom-Json -AsHashtable -Depth 100
        $runtimeIdentityTamper.buildIdentity.headCommit = 'f' * 40
        $runtimeIdentityTamper.buildIdentityFingerprintSha256 = Get-TextSha256 -Text `
            ($runtimeIdentityTamper.buildIdentity | ConvertTo-Json -Depth 16 -Compress)
        [System.IO.File]::WriteAllText($packageManifestPath,
            ($runtimeIdentityTamper | ConvertTo-Json -Depth 100), [System.Text.UTF8Encoding]::new($false))
        $runtimeIdentityVerification = Invoke-Tool -Script $packageVerifier -Arguments @(
            '-PackageDirectory', [string]$packageCreateResult.packageDirectory)
        Require-Failure -Result $runtimeIdentityVerification -Name 'packaged application runtime-identity guard' `
            -Pattern 'runtime build identity does not match package HEAD/protocol'
    }
    finally
    {
        [System.IO.File]::WriteAllText($packageManifestPath, $originalPackageManifestText,
            [System.Text.UTF8Encoding]::new($false))
    }
    $packageManifestHashBeforeOverlapProbe = (Get-FileHash -LiteralPath $packageManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $overlappingKitCreation = Invoke-Tool -Script $kitCreator -Arguments @(
        '-PackageDirectory', [string]$packageCreateResult.packageDirectory,
        '-PackageSealPath', [string]$packageCreateResult.sealPath,
        '-PackageArchivePath', [string]$packageCreateResult.archivePath,
        '-ExpectedPackageManifestSha256', [string]$packageCreateResult.manifestSha256,
        '-SourceSetDirectory', $sourceSetDirectory,
        '-SourceSetSealPath', $sourceSealPath,
        '-ExpectedSourceManifestSha256', $manifestHash,
        '-ExpectedHeadCommit', $headCommit,
        '-ComputerBProtectedMonitorDeviceName', '\\.\DISPLAY2',
        '-ComputerBExperimentMonitorDeviceName', '\\.\DISPLAY1',
        '-OutputRoot', [string]$packageCreateResult.packageDirectory)
    Require-Failure -Result $overlappingKitCreation -Name 'Computer B kit input/output overlap guard' `
        -Pattern 'output must remain outside the input package and source-set directories'
    if ((Get-FileHash -LiteralPath $packageManifestPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        $packageManifestHashBeforeOverlapProbe)
    {
        throw 'Rejected Computer B kit overlap changed its package input'
    }

    $archiveStream = [System.IO.File]::Open([string]$kitCreateResult.archivePath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try
    {
        $originalArchiveByte = $archiveStream.ReadByte()
        if ($originalArchiveByte -lt 0)
        {
            throw 'Computer B kit archive is unexpectedly empty'
        }
        $archiveStream.Position = 0
        $archiveStream.WriteByte([byte]($originalArchiveByte -bxor 0xff))
        $archiveStream.Flush($true)
    }
    finally
    {
        $archiveStream.Dispose()
    }
    $tamperedArchiveVerification = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-KitSealPath', [string]$kitCreateResult.sealPath,
        '-ArchivePath', [string]$kitCreateResult.archivePath,
        '-ExpectedManifestSha256', [string]$kitCreateResult.manifestSha256)
    Require-Failure -Result $tamperedArchiveVerification -Name 'Computer B kit archive-tamper guard' `
        -Pattern 'archive identity differs from its seal'
    $archiveStream = [System.IO.File]::Open([string]$kitCreateResult.archivePath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try
    {
        $archiveStream.WriteByte([byte]$originalArchiveByte)
        $archiveStream.Flush($true)
    }
    finally
    {
        $archiveStream.Dispose()
    }
    if ((Get-FileHash -LiteralPath ([string]$kitCreateResult.archivePath) -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        [string]$kitCreateResult.archiveSha256)
    {
        throw 'Computer B kit archive-tamper fixture did not restore the original archive identity'
    }

    $demoBatchPath = Join-Path ([string]$kitCreateResult.kitDirectory) 'Start-PBRemoteVisualExperimentMonitorDemo.bat'
    $previousPreflightValue = $env:PB_PACKAGE_PREFLIGHT_ONLY
    $demoPreflightOutput = ''
    $demoPreflightExitCode = -1
    try
    {
        $env:PB_PACKAGE_PREFLIGHT_ONLY = '1'
        $demoPreflightOutput = @(& $env:ComSpec '/d' '/c' ('"' + $demoBatchPath + '"') 2>&1) -join "`n"
        $demoPreflightExitCode = $LASTEXITCODE
    }
    finally
    {
        $env:PB_PACKAGE_PREFLIGHT_ONLY = $previousPreflightValue
    }
    if ($demoPreflightExitCode -ne 0 -or $demoPreflightOutput -notmatch 'PACKAGE PREFLIGHT PASSED')
    {
        throw "Computer B one-click demo preflight did not find the packaged Encoder and test binary: $demoPreflightOutput"
    }
    $previousPreflightValue = $env:PB_PACKAGE_PREFLIGHT_ONLY
    $launchPreflightOutput = ''
    $launchPreflightExitCode = -1
    try
    {
        $env:PB_PACKAGE_PREFLIGHT_ONLY = '2'
        $launchPreflightOutput = @(& $env:ComSpec '/d' '/c' ('"' + $demoBatchPath + '"') 2>&1) -join "`n"
        $launchPreflightExitCode = $LASTEXITCODE
    }
    finally
    {
        $env:PB_PACKAGE_PREFLIGHT_ONLY = $previousPreflightValue
    }
    if ($launchPreflightExitCode -ne 0 -or $launchPreflightOutput -notmatch 'LAUNCH PREFLIGHT PASSED' -or
        $launchPreflightOutput -notmatch 'Demo RunId: [0-9a-f]{32}')
    {
        throw "Computer B one-click demo RunId/create-only preflight failed: $launchPreflightOutput"
    }

    $wrongKitManifest = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-ExpectedManifestSha256', ('0' * 64))
    Require-Failure -Result $wrongKitManifest -Name 'Computer B kit expected-manifest guard' `
        -Pattern 'manifest size or expected SHA-256 is invalid'

    $workingDirectory = Join-Path ([string]$kitCreateResult.kitDirectory) 'Working'
    if (-not (Test-Path -LiteralPath $workingDirectory -PathType Container))
    {
        [void](New-Item -ItemType Directory -Path $workingDirectory)
    }
    [System.IO.File]::WriteAllText((Join-Path $workingDirectory 'allowed-runtime-output.txt'), 'allowed')
    $workingVerificationPath = Join-Path $workingDirectory 'verification.json'
    $workingOutputVerification = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-ExpectedManifestSha256', [string]$kitCreateResult.manifestSha256,
        '-OutputPath', $workingVerificationPath)
    Require-Success -Result $workingOutputVerification -Name 'Computer B kit mutable Working-directory allowance'
    if (-not (Test-Path -LiteralPath $workingVerificationPath -PathType Leaf))
    {
        throw 'Computer B kit verifier did not publish its create-only result below Working'
    }

    $immutableVerificationPath = Join-Path ([string]$kitCreateResult.kitDirectory) 'invalid-verification-output.json'
    $immutableOutputVerification = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-ExpectedManifestSha256', [string]$kitCreateResult.manifestSha256,
        '-OutputPath', $immutableVerificationPath)
    Require-Failure -Result $immutableOutputVerification -Name 'Computer B kit immutable-output guard' `
        -Pattern 'outside the immutable kit or below Working'
    if (Test-Path -LiteralPath $immutableVerificationPath)
    {
        throw 'Rejected Computer B kit verification output mutated the immutable kit'
    }

    $unexpectedFilePath = Join-Path ([string]$kitCreateResult.kitDirectory) 'unexpected-immutable-file.txt'
    [System.IO.File]::WriteAllText($unexpectedFilePath, 'unexpected')
    $unexpectedFileVerification = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-ExpectedManifestSha256', [string]$kitCreateResult.manifestSha256)
    Require-Failure -Result $unexpectedFileVerification -Name 'Computer B kit unmanifested-file guard' `
        -Pattern 'immutable file count mismatch'
    [System.IO.File]::Delete($unexpectedFilePath)

    $demoSourcePath = Join-Path (Join-Path ([string]$kitCreateResult.kitDirectory) 'source-set') 'random-1MiB.bin'
    $demoSourceStream = [System.IO.File]::Open($demoSourcePath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try
    {
        $originalByte = $demoSourceStream.ReadByte()
        $demoSourceStream.Position = 0
        $demoSourceStream.WriteByte([byte]($originalByte -bxor 0xff))
        $demoSourceStream.Flush($true)
    }
    finally
    {
        $demoSourceStream.Dispose()
    }
    $tamperedSourceVerification = Invoke-Tool -Script $kitVerifier -Arguments @(
        '-KitDirectory', [string]$kitCreateResult.kitDirectory,
        '-ExpectedManifestSha256', [string]$kitCreateResult.manifestSha256)
    Require-Failure -Result $tamperedSourceVerification -Name 'Computer B kit test-binary tamper guard' `
        -Pattern 'immutable file identity mismatch'
    $demoSourceStream = [System.IO.File]::Open($demoSourcePath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try
    {
        $demoSourceStream.WriteByte([byte]$originalByte)
        $demoSourceStream.Flush($true)
    }
    finally
    {
        $demoSourceStream.Dispose()
    }

    $metadataPath = Join-Path $runRoot 'remote-metadata.json'
    $metadataCreation = Invoke-Tool -Script $presetCreator -Arguments @(
        '-OutputPath', $metadataPath,
        '-RemoteProvider', 'AutomatedHeadlessFixture',
        '-RemoteProviderVersion', '1',
        '-RemoteMode', 'EvidenceContractTest',
        '-TargetFps', '5')
    Require-Success -Result $metadataCreation -Name 'RemoteVisual metadata creation'
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ($metadata.schema -cne 'PixelBridge.RemoteVisualRunMetadata.1' -or
        $metadata.runId -cnotmatch '^[0-9a-f]{32}$')
    {
        throw 'Generated RemoteVisual metadata identity is invalid'
    }
    $metadataHash = (Get-FileHash -LiteralPath $metadataPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $duplicateMetadata = Invoke-Tool -Script $presetCreator -Arguments @(
        '-OutputPath', $metadataPath,
        '-RemoteProvider', 'AutomatedHeadlessFixture')
    Require-Failure -Result $duplicateMetadata -Name 'metadata create-only guard' -Pattern 'already exists'
    if ((Get-FileHash -LiteralPath $metadataPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $metadataHash)
    {
        throw 'Rejected metadata overwrite changed the original artifact'
    }

    Write-Output 'PBRemoteVisual deployment-evidence contracts: PASS'
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

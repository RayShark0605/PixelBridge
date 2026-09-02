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
    $packageOutputRoot = Join-Path $runRoot 'portable-package'
    [void](New-Item -ItemType Directory -Path $packageOutputRoot)
    $packageCreation = Invoke-Tool -Script $packageCreator -Arguments @(
        '-Role', 'Both',
        '-Label', 'current-head',
        '-BuildDirectory', $resolvedBuildDirectory,
        '-OutputRoot', $packageOutputRoot,
        '-ExcludedSourcePath', 'docs/PHASE1_GATE_REPORT.md')
    Require-Success -Result $packageCreation -Name 'Both-role portable-package creation for Computer B kit'
    $packageCreateResult = $packageCreation.output | ConvertFrom-Json
    if ($packageCreateResult.packageDirectory -isnot [string] -or
        $packageCreateResult.manifestSha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw 'Portable-package creator did not return a valid Computer B kit input identity'
    }

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

    $packageManifestPath = Join-Path ([string]$packageCreateResult.packageDirectory) 'package-manifest.json'
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

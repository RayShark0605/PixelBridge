#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackageSealPath,

    [Parameter(Mandatory = $true)]
    [string]$PackageArchivePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedPackageManifestSha256,

    [Parameter(Mandatory = $true)]
    [string]$SourceSetDirectory,

    [Parameter(Mandatory = $true)]
    [string]$SourceSetSealPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedSourceManifestSha256,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ExpectedHeadCommit,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\\\\\.\\DISPLAY[1-9][0-9]*$')]
    [string]$ComputerBProtectedMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\\\\\.\\DISPLAY[1-9][0-9]*$')]
    [string]$ComputerBExperimentMonitorDeviceName,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [ValidateRange(1, 5)]
    [UInt32]$DemoLogicalFps = 2,

    [ValidateRange(1, 64)]
    [UInt32]$DemoControlRepetitions = 12
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$commonModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop

function Write-NewText
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [switch]$Ascii
    )
    $encoding = if ($Ascii) { [System.Text.ASCIIEncoding]::new() } else { [System.Text.UTF8Encoding]::new($false) }
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, $encoding)
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

function ConvertTo-CmdText
{
    param([Parameter(Mandatory = $true)][string]$Text)
    return ([regex]::Replace($Text, '\r?\n', "`r`n")).TrimStart([char[]]"`r`n")
}

function Test-SafeBatchComponent
{
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -and -not $Value.EndsWith('.')
}

function Test-IsSameOrDescendantPath
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $resolvedPath.Equals($resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $resolvedPath.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)
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

function Get-OrdinallySortedFiles
{
    param([Parameter(Mandatory = $true)][object[]]$Files)
    $sortedFiles = [object[]]@($Files)
    $comparison = [System.Comparison[object]]{
        param($left, $right)
        return [string]::CompareOrdinal([string]$left.path, [string]$right.path)
    }
    [Array]::Sort[object]($sortedFiles, $comparison)
    return @($sortedFiles)
}

function Get-CanonicalFingerprint
{
    param([Parameter(Mandatory = $true)][object[]]$Files)
    $canonical = (@(Get-OrdinallySortedFiles -Files $Files) | ForEach-Object {
        "$($_.path)`0$($_.size)`0$($_.sha256)`n"
    }) -join ''
    return Get-TextSha256 -Text $canonical
}

function Get-PortablePackageCompatibleFingerprint
{
    param([Parameter(Mandatory = $true)][object[]]$Files)
    $canonical = (@($Files | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable) | ForEach-Object {
        "$($_.path)`0$($_.size)`0$($_.sha256)`n"
    }) -join ''
    return Get-TextSha256 -Text $canonical
}

function Get-CurrentSourceFingerprint
{
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
    $sourcePaths = @(& git -C $RepositoryRoot -c core.quotepath=false ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Unable to enumerate the current source tree for Computer B kit binding'
    }
    $inventory = [Collections.Generic.List[object]]::new()
    $repositoryPrefix = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\') + '\'
    foreach ($sourcePath in @($sourcePaths | Sort-Object -CaseSensitive))
    {
        $relativePath = $sourcePath.Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($relativePath) -or $relativePath -ceq 'docs/PHASE1_GATE_REPORT.md')
        {
            continue
        }
        if ($relativePath.Contains("`r") -or $relativePath.Contains("`n"))
        {
            throw 'Computer B kit source identity does not support a path containing a line break'
        }
        $absolutePath = [System.IO.Path]::GetFullPath((Join-Path $RepositoryRoot $sourcePath))
        if (-not $absolutePath.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $absolutePath -PathType Leaf))
        {
            throw "Computer B kit source identity contains an invalid path: $relativePath"
        }
        $item = Get-Item -LiteralPath $absolutePath
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "Computer B kit source identity contains a reparse point: $relativePath"
        }
        [void]$inventory.Add([ordered]@{
            path = $relativePath
            size = [UInt64]$item.Length
            sha256 = (Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
    if ($inventory.Count -eq 0 -or $inventory.Count -gt 65536)
    {
        throw 'Computer B kit source inventory is outside the bounded range'
    }
    return Get-PortablePackageCompatibleFingerprint -Files @($inventory)
}

function Copy-RegularTree
{
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $sourceItem = Get-Item -LiteralPath $Source -Force
    if (-not $sourceItem.PSIsContainer -or ($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Computer B kit tree source is not a regular directory: $Source"
    }
    $reparsePoint = Get-ChildItem -LiteralPath $Source -Force -Recurse | Where-Object {
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
    } | Select-Object -First 1
    if ($null -ne $reparsePoint)
    {
        throw "Computer B kit tree source contains a reparse point: $($reparsePoint.FullName)"
    }
    [void](New-Item -ItemType Directory -Path $Destination)
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse
    }
}

function Copy-RepositoryFiles
{
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string[]]$Paths,
        [Parameter(Mandatory = $true)][string]$DestinationRoot
    )
    $repositoryPrefix = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\') + '\'
    foreach ($relativePath in $Paths)
    {
        $source = [System.IO.Path]::GetFullPath((Join-Path $RepositoryRoot $relativePath))
        if (-not $source.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $source -PathType Leaf))
        {
            throw "Computer B operator-tool source path is invalid: $relativePath"
        }
        $sourceItem = Get-Item -LiteralPath $source
        if (($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "Computer B operator-tool source is a reparse point: $relativePath"
        }
        $destination = Join-Path $DestinationRoot $relativePath
        $destinationParent = [System.IO.Path]::GetDirectoryName($destination)
        if (-not (Test-Path -LiteralPath $destinationParent -PathType Container))
        {
            [void](New-Item -ItemType Directory -Path $destinationParent -Force)
        }
        Copy-Item -LiteralPath $source -Destination $destination
    }
}

function Get-ImmutableFiles
{
    param([Parameter(Mandatory = $true)][string]$Root)
    $rootPrefix = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $files = [Collections.Generic.List[object]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Force -Recurse)
    {
        $fullPath = [System.IO.Path]::GetFullPath($file.FullName)
        if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Computer B kit file escaped its root: $fullPath"
        }
        $relativePath = $fullPath.Substring($rootPrefix.Length).Replace('\', '/')
        if ($relativePath -ceq 'COMPUTER-B-KIT-MANIFEST.json' -or
            $relativePath.StartsWith('Working/', [StringComparison]::OrdinalIgnoreCase))
        {
            continue
        }
        if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or $file.Length -eq 0 -or
            [UInt64]$file.Length -gt 2GB)
        {
            throw "Computer B kit immutable file is empty, oversized, or a reparse point: $relativePath"
        }
        [void]$files.Add([ordered]@{
            path = $relativePath
            size = [UInt64]$file.Length
            sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
    return @(Get-OrdinallySortedFiles -Files @($files))
}

function Invoke-StrictJsonTool
{
    param(
        [Parameter(Mandatory = $true)][string]$Script,
        [Parameter(Mandatory = $true)][hashtable]$Arguments,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $text = @(& $Script @Arguments 2>&1) -join "`n"
    return ConvertFrom-PBStrictJsonText -Text $text -Name "$Name output"
}

if ($ComputerBProtectedMonitorDeviceName -ieq $ComputerBExperimentMonitorDeviceName)
{
    throw 'Computer B kit requires distinct Protected and Experiment monitor device names'
}
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$resolvedPackage = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$resolvedPackageSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
$resolvedPackageArchive = [System.IO.Path]::GetFullPath($PackageArchivePath)
$resolvedSourceSet = [System.IO.Path]::GetFullPath($SourceSetDirectory).TrimEnd('\')
$resolvedSourceSetSeal = [System.IO.Path]::GetFullPath($SourceSetSealPath)
$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
if (-not (Test-Path -LiteralPath $resolvedOutputRoot -PathType Container) -or
    ((Get-Item -LiteralPath $resolvedOutputRoot).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
{
    throw 'Computer B kit output root must be an existing regular directory'
}

$packageVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
$packageVerification = Invoke-StrictJsonTool -Script $packageVerifier -Arguments @{
    PackageDirectory = $resolvedPackage
    PackageSealPath = $resolvedPackageSeal
    ArchivePath = $resolvedPackageArchive
    ExpectedManifestSha256 = $ExpectedPackageManifestSha256
} -Name 'portable-package verification'
if ($packageVerification.verified -isnot [bool] -or -not [bool]$packageVerification.verified)
{
    throw 'Computer B kit input package did not pass verification'
}
$packageManifestPath = Join-Path $resolvedPackage 'package-manifest.json'
$packageManifest = Read-PBBoundedJson -Path $packageManifestPath -MaximumBytes 32MB
$packageDirectoryName = [System.IO.Path]::GetFileName($resolvedPackage)
if ([string]$packageManifest.packageName -cne $packageDirectoryName -or
    [string]$packageManifest.buildIdentity.headCommit -cne $ExpectedHeadCommit -or
    @($packageManifest.endpointRoles).Count -ne 2 -or
    [string]$packageManifest.endpointRoles[0] -cne 'Encoder' -or
    [string]$packageManifest.endpointRoles[1] -cne 'Decoder' -or
    [string]$packageVerification.packagePayloadFingerprintSha256 -cne
        [string]$packageManifest.packagePayloadFingerprintSha256)
{
    throw 'Computer B kit requires the exact expected Both-role current-head package'
}
$currentHeadCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
$currentHeadTree = (& git -C $repositoryRoot rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0 -or $currentHeadCommit -cne $ExpectedHeadCommit -or
    $currentHeadTree -cne [string]$packageManifest.buildIdentity.headTree)
{
    throw 'Computer B kit package HEAD/tree differs from the current source checkout'
}
$currentSourceFingerprint = Get-CurrentSourceFingerprint -RepositoryRoot $repositoryRoot
if ($currentSourceFingerprint -cne [string]$packageManifest.buildIdentity.testedSourceFingerprintSha256)
{
    throw 'Computer B kit package tested-source fingerprint differs from the current checkout'
}

$sourceVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualSourceSet.ps1'
$sourceVerification = Invoke-StrictJsonTool -Script $sourceVerifier -Arguments @{
    SourceSetDirectory = $resolvedSourceSet
    SourceSealPath = $resolvedSourceSetSeal
    ExpectedManifestSha256 = $ExpectedSourceManifestSha256
} -Name 'source-set verification'
if ($sourceVerification.verified -isnot [bool] -or -not [bool]$sourceVerification.verified)
{
    throw 'Computer B kit input source set did not pass verification'
}
$sourceManifest = Read-PBBoundedJson -Path (Join-Path $resolvedSourceSet 'source-manifest.json') -MaximumBytes 2MB
$sourceSetDirectoryName = [System.IO.Path]::GetFileName($resolvedSourceSet)
$demoSources = @($sourceManifest.files | Where-Object { [string]$_.path -ceq 'random-1MiB.bin' })
if (-not (Test-SafeBatchComponent -Value $packageDirectoryName) -or
    -not (Test-SafeBatchComponent -Value $sourceSetDirectoryName) -or
    [string]$sourceManifest.sourceSetName -cne $sourceSetDirectoryName -or $demoSources.Count -ne 1 -or
    [UInt64]$demoSources[0].size -ne 1MB -or [string]$demoSources[0].sha256 -cnotmatch '^[0-9a-f]{64}$')
{
    throw 'Computer B kit package/source names or sealed 1 MiB demo source are invalid'
}
$demoSource = $demoSources[0]

$operatorSourcePaths = @(& git -C $repositoryRoot -c core.quotepath=false ls-files --cached --others `
    --exclude-standard -- 'tools/PBRemoteVisualEvidence' 'tools/PBRemoteVisualReport')
if ($LASTEXITCODE -ne 0 -or $operatorSourcePaths.Count -eq 0 -or $operatorSourcePaths.Count -gt 4096)
{
    throw 'Unable to enumerate the bounded Computer B operator-tool source set'
}
$operatorSourcePaths = @($operatorSourcePaths | Sort-Object -CaseSensitive -Unique)
$documentationPaths = @(
    'docs/CURRENT_RUNTIME_OPTION_INVENTORY.md',
    'docs/REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md',
    'docs/REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md',
    'docs/REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md')

$packageFingerprintPrefix = ([string]$packageManifest.packagePayloadFingerprintSha256).Substring(0, 8)
$sourceFingerprintPrefix = ([string]$sourceManifest.sourceSetFingerprintSha256).Substring(0, 8)
$kitName = "PB-S21B-$($ExpectedHeadCommit.Substring(0, 8))-$packageFingerprintPrefix-$sourceFingerprintPrefix"
$finalDirectory = Join-Path $resolvedOutputRoot $kitName
$stagingDirectory = "$finalDirectory.partial"
$archivePath = Join-Path $resolvedOutputRoot "$kitName.zip"
$temporaryArchivePath = Join-Path $resolvedOutputRoot "$kitName.partial.zip"
$sealPath = Join-Path $resolvedOutputRoot "$kitName.seal.json"
foreach ($path in @($finalDirectory, $stagingDirectory, $archivePath, $temporaryArchivePath, $sealPath))
{
    if ((Test-IsSameOrDescendantPath -Path $path -Root $resolvedPackage) -or
        (Test-IsSameOrDescendantPath -Path $path -Root $resolvedSourceSet))
    {
        throw 'Computer B kit output must remain outside the input package and source-set directories'
    }
}
foreach ($path in @($finalDirectory, $stagingDirectory, $archivePath, $temporaryArchivePath, $sealPath))
{
    if (Test-Path -LiteralPath $path)
    {
        throw "Computer B kit create-only output already exists: $path"
    }
}

$published = $false
try
{
    [void](New-Item -ItemType Directory -Path $stagingDirectory)
    Copy-RegularTree -Source $resolvedPackage -Destination (Join-Path $stagingDirectory $packageDirectoryName)
    Copy-Item -LiteralPath $resolvedPackageSeal -Destination (Join-Path $stagingDirectory "$packageDirectoryName.seal.json")
    Copy-RegularTree -Source $resolvedSourceSet -Destination (Join-Path $stagingDirectory $sourceSetDirectoryName)
    Copy-Item -LiteralPath $resolvedSourceSetSeal -Destination (Join-Path $stagingDirectory "$sourceSetDirectoryName.seal.json")

    $operatorRoot = Join-Path $stagingDirectory 'operator-tools'
    [void](New-Item -ItemType Directory -Path $operatorRoot)
    Copy-RepositoryFiles -RepositoryRoot $repositoryRoot -Paths $operatorSourcePaths -DestinationRoot $operatorRoot
    Copy-RepositoryFiles -RepositoryRoot $repositoryRoot -Paths $documentationPaths -DestinationRoot $operatorRoot

    $verifyBatch = ConvertTo-CmdText -Text @"
@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
where pwsh.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: PowerShell 7 pwsh.exe is required for the formal kit verifier.
    pause
    exit /b 2
)
pwsh.exe -NoLogo -NoProfile -NonInteractive -File "%~dp0operator-tools\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualStep21ComputerBKit.ps1" -KitDirectory "%~dp0."
set "VERIFY_EXIT=%ERRORLEVEL%"
if "%VERIFY_EXIT%"=="0" (
    echo.
    echo COMPUTER B KIT VERIFICATION PASSED.
) else (
    echo.
    echo COMPUTER B KIT VERIFICATION FAILED with exit code %VERIFY_EXIT%.
)
pause
exit /b %VERIFY_EXIT%
"@
    Write-NewText -Path (Join-Path $stagingDirectory 'VERIFY-COMPUTER-B-KIT.bat') -Text $verifyBatch -Ascii

    $captureBatch = ConvertTo-CmdText -Text @"
@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
set "DECODER=%~dp0$packageDirectoryName\Decoder\PixelBridgeDecoder.exe"
set "WORKING=%~dp0Working"
set "OUTPUT=%WORKING%\computer-b-monitor-catalog.json"
set "PARTIAL=%OUTPUT%.partial"
if not exist "%DECODER%" (
    echo ERROR: Decoder monitor-catalog executable is missing: %DECODER%
    pause
    exit /b 2
)
if exist "%OUTPUT%" (
    echo ERROR: Create-only monitor catalog already exists: %OUTPUT%
    pause
    exit /b 2
)
if not exist "%WORKING%" mkdir "%WORKING%" >nul 2>&1
if exist "%PARTIAL%" (
    echo ERROR: Create-only monitor catalog partial already exists: %PARTIAL%
    pause
    exit /b 2
)
"%DECODER%" --list-monitors > "%PARTIAL%"
set "CATALOG_EXIT=%ERRORLEVEL%"
if not "%CATALOG_EXIT%"=="0" (
    if exist "%PARTIAL%" del /f /q "%PARTIAL%" >nul 2>&1
    echo ERROR: monitor catalog failed with exit code %CATALOG_EXIT%.
    pause
    exit /b %CATALOG_EXIT%
)
if exist "%OUTPUT%" (
    echo ERROR: monitor catalog destination appeared while capture was running; partial retained: %PARTIAL%
    pause
    exit /b 2
)
move "%PARTIAL%" "%OUTPUT%" >nul
if errorlevel 1 (
    echo ERROR: failed to publish the create-only monitor catalog; partial retained: %PARTIAL%
    pause
    exit /b 2
)
echo Computer B monitor catalog created without GUI or display mutation:
echo %OUTPUT%
pause
exit /b 0
"@
    Write-NewText -Path (Join-Path $stagingDirectory 'Capture-ComputerBMonitorCatalog.bat') -Text $captureBatch -Ascii

    $demoBatch = ConvertTo-CmdText -Text @"
@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
set "ENCODER=%~dp0$packageDirectoryName\Encoder\PixelBridgeEncoder.exe"
set "SOURCE=%~dp0$sourceSetDirectoryName\random-1MiB.bin"
set "EXPECTED_SHA256=$([string]$demoSource.sha256)"
set "CERTUTIL=%SystemRoot%\System32\certutil.exe"
if not exist "%CERTUTIL%" (
    echo ERROR: Windows certutil.exe is unavailable for SHA-256 verification.
    pause
    exit /b 2
)
if not exist "%ENCODER%" (
    echo ERROR: PixelBridgeEncoder.exe is missing from the single extracted kit.
    echo Expected: %ENCODER%
    pause
    exit /b 2
)
if not exist "%SOURCE%" (
    echo ERROR: random-1MiB.bin is missing from the single extracted kit.
    echo Expected: %SOURCE%
    pause
    exit /b 2
)
set "ACTUAL_SHA256="
for /f "skip=1 tokens=*" %%H in ('%CERTUTIL% -hashfile "%SOURCE%" SHA256') do if not defined ACTUAL_SHA256 set "ACTUAL_SHA256=%%H"
set "ACTUAL_SHA256=%ACTUAL_SHA256: =%"
if /i not "%ACTUAL_SHA256%"=="%EXPECTED_SHA256%" (
    echo ERROR: random-1MiB.bin SHA-256 mismatch.
    echo Expected: %EXPECTED_SHA256%
    echo Actual:   %ACTUAL_SHA256%
    pause
    exit /b 2
)
if /i "%PB_PACKAGE_PREFLIGHT_ONLY%"=="1" (
    echo PACKAGE PREFLIGHT PASSED.
    echo Encoder: %ENCODER%
    echo Source SHA-256: %ACTUAL_SHA256%
    exit /b 0
)
set "ID_SHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%ID_SHELL%" (
    set "ID_SHELL="
    for /f "delims=" %%P in ('where pwsh.exe 2^>nul') do if not defined ID_SHELL set "ID_SHELL=%%P"
)
if not defined ID_SHELL (
    echo ERROR: neither Windows PowerShell nor PowerShell 7 is available for RunId creation.
    pause
    exit /b 2
)
set "RUN_ID="
for /f "delims=" %%I in ('call "%ID_SHELL%" -NoLogo -NoProfile -NonInteractive -Command "[Guid]::NewGuid().ToString('N')"') do set "RUN_ID=%%I"
if not defined RUN_ID (
    echo ERROR: a fresh demo RunId could not be created.
    pause
    exit /b 2
)
set "RUN_DIR=%~dp0Working\demo-runs\%RUN_ID%"
if exist "%RUN_DIR%" (
    echo ERROR: generated demo RunId already has an evidence directory: %RUN_DIR%
    pause
    exit /b 2
)
mkdir "%RUN_DIR%" >nul 2>&1
if not exist "%RUN_DIR%" (
    echo ERROR: failed to create demo run directory: %RUN_DIR%
    pause
    exit /b 2
)
if /i "%PB_PACKAGE_PREFLIGHT_ONLY%"=="2" (
    echo LAUNCH PREFLIGHT PASSED.
    echo Demo RunId: %RUN_ID%
    echo Evidence directory: %RUN_DIR%
    exit /b 0
)
echo PixelBridge RemoteVisual LF4 ExperimentMonitor demo
echo This launcher is the user-authorized single-run Step 21 acceptance candidate.
echo Fullscreen output selector: primary
echo The current Windows primary monitor is selected at launch time.
echo Source SHA-256: %ACTUAL_SHA256%
echo Demo RunId: %RUN_ID%
echo.
echo Wait for Computer A Decoder WholeFileDigest and safe publish, then press Q or Enter.
"%ENCODER%" --headless-broadcast --source "%SOURCE%" --profile remote-lf4 --channel remote --remote-provider UserProvidedVisualLink --compression off --single-monitor-fullscreen primary --logical-fps $DemoLogicalFps --control-repetitions $DemoControlRepetitions --manual-stop --loop --run-id "%RUN_ID%" --journal "%RUN_DIR%\encoder-journal.ndjson" --report "%RUN_DIR%\encoder-report.json"
set "ENCODER_EXIT=%ERRORLEVEL%"
echo.
if "%ENCODER_EXIT%"=="0" (
    echo Encoder stopped cleanly. Demo evidence: %RUN_DIR%
) else (
    echo Encoder failed with exit code %ENCODER_EXIT%.
)
pause
exit /b %ENCODER_EXIT%
"@
    Write-NewText -Path (Join-Path $stagingDirectory 'Start-PBRemoteVisualExperimentMonitorDemo.bat') `
        -Text $demoBatch -Ascii

    $readme = @"
PixelBridge RemoteVisual Step 21 - Computer B single-extraction kit
===================================================================

This directory is complete after one extraction. Do not copy a BAT by itself and do not perform a second nested package extraction.

Immediate checks:
1. Double-click VERIFY-COMPUTER-B-KIT.bat. It verifies every immutable file, the expanded Both-role runtime package, and the CSPRNG source set.
2. Double-click Capture-ComputerBMonitorCatalog.bat. It performs a read-only packaged Decoder --list-monitors query and creates Working\computer-b-monitor-catalog.json without opening a GUI or changing display state.
3. Confirm Computer B still has two non-overlapping Extended Desktop monitors. Frozen readiness roles for this kit are:
   ProtectedMonitor: $ComputerBProtectedMonitorDeviceName
   ExperimentMonitor: $ComputerBExperimentMonitorDeviceName

Ad-hoc recovery demonstration:
- Start-PBRemoteVisualExperimentMonitorDemo.bat uses the real production Encoder at:
  $packageDirectoryName\Encoder\PixelBridgeEncoder.exe
- It uses the sealed 1 MiB test binary at:
  $sourceSetDirectoryName\random-1MiB.bin
- Test binary SHA-256: $([string]$demoSource.sha256)
- The demo resolves Windows' unique primary monitor at launch time instead of assuming a DEVICE number. The fullscreen outer surface covers that monitor. The logical LF4 canvas remains centered and locator-derived geometry on Computer A is authoritative, so the captured/player/remote picture need not be 16:9 and need not use a pre-assumed scale.
- It loops until Q or Enter. Stop only after Computer A reports Receiver completion, WholeFileDigest verification, safe publish, and an external byte-exact comparison.

Formal Step 21 boundary:
- The convenience demo BAT is the current user-authorized single-run acceptance candidate; it does not claim cross-provider or cross-mode matrix coverage.
- For each formal cell, Computer A must first create a fresh RunId, deployment/UI evidence and PilotPlan.3, then start the Decoder before invoking operator-tools\tools\PBRemoteVisualEvidence\Invoke-PBRemoteVisualPilotEncoder.ps1 on Computer B.
- Do not transfer payload through clipboard, shared folders, sockets, temp files, or any route other than the displayed pixels during a formal run.
- This kit alone is PREDEPLOYMENT_ONLY: Step 21 is accepted only after Computer A completes production Receiver recovery, WholeFileDigest verification, safe publish, and an external byte-exact comparison. No Certified Profile is implied.

Package HEAD: $ExpectedHeadCommit
Package manifest SHA-256: $ExpectedPackageManifestSha256
Source manifest SHA-256: $ExpectedSourceManifestSha256
"@
    Write-NewText -Path (Join-Path $stagingDirectory 'README-COMPUTER-B.txt') -Text $readme

    $immutableFiles = @(Get-ImmutableFiles -Root $stagingDirectory)
    $operatorToolFiles = @($immutableFiles | Where-Object { [string]$_.path -clike 'operator-tools/*' })
    if ($immutableFiles.Count -eq 0 -or $immutableFiles.Count -gt 4096 -or $operatorToolFiles.Count -eq 0)
    {
        throw 'Computer B kit immutable or operator-tool inventory is outside the bounded range'
    }
    $payloadFingerprint = Get-CanonicalFingerprint -Files $immutableFiles
    $operatorToolsFingerprint = Get-CanonicalFingerprint -Files $operatorToolFiles
    $manifest = [ordered]@{
        schema = 'PixelBridge.RemoteVisualStep21ComputerBKit.1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        status = 'PREDEPLOYMENT_ONLY'
        kitName = $kitName
        headCommit = $ExpectedHeadCommit
        headTree = [string]$packageManifest.buildIdentity.headTree
        package = [ordered]@{
            directoryName = $packageDirectoryName
            sealFileName = "$packageDirectoryName.seal.json"
            manifestSha256 = $ExpectedPackageManifestSha256
            payloadFingerprintSha256 = [string]$packageManifest.packagePayloadFingerprintSha256
            endpointRoles = @('Encoder', 'Decoder')
        }
        sourceSet = [ordered]@{
            directoryName = $sourceSetDirectoryName
            sealFileName = "$sourceSetDirectoryName.seal.json"
            manifestSha256 = $ExpectedSourceManifestSha256
            sourceSetId = [string]$sourceManifest.sourceSetId
            sourceSetFingerprintSha256 = [string]$sourceManifest.sourceSetFingerprintSha256
            demoSource = [ordered]@{
                path = 'random-1MiB.bin'
                size = [UInt64]$demoSource.size
                sha256 = [string]$demoSource.sha256
            }
        }
        operatorTools = [ordered]@{
            root = 'operator-tools'
            fileCount = [UInt32]$operatorToolFiles.Count
            fingerprintSha256 = $operatorToolsFingerprint
        }
        launchers = [ordered]@{
            verify = 'VERIFY-COMPUTER-B-KIT.bat'
            captureMonitorCatalog = 'Capture-ComputerBMonitorCatalog.bat'
            experimentMonitorDemo = 'Start-PBRemoteVisualExperimentMonitorDemo.bat'
            demoProfile = 'remote-lf4'
            demoLogicalFps = $DemoLogicalFps
            demoControlRepetitions = $DemoControlRepetitions
            demoMonitorSelector = 'primary'
            computerBProtectedMonitor = $ComputerBProtectedMonitorDeviceName
            computerBExperimentMonitor = $ComputerBExperimentMonitorDeviceName
        }
        truthBoundary = [ordered]@{
            formalStep21Accepted = $false
            executedCellCount = [UInt32]0
            certifiedRemoteVisualProfile = $false
            demoLauncherIsFormalCell = $false
            providerUiEvidenceStillRequiredPerRun = $true
            freshRunIdAndPilotPlanStillRequiredPerCell = $true
            statement = 'This is a predeployment Computer B kit. It allocates no formal RunId, executes no Step 21 cell, and does not certify a RemoteVisual profile.'
        }
        fileCount = [UInt32]$immutableFiles.Count
        files = $immutableFiles
        payloadFingerprintSha256 = $payloadFingerprint
    }
    $manifestPath = Join-Path $stagingDirectory 'COMPUTER-B-KIT-MANIFEST.json'
    $manifestIdentity = Write-PBCreateOnlyJson -Path $manifestPath -Value $manifest -Depth 40

    if ((Get-CurrentSourceFingerprint -RepositoryRoot $repositoryRoot) -cne $currentSourceFingerprint -or
        (& git -C $repositoryRoot rev-parse HEAD).Trim() -cne $ExpectedHeadCommit)
    {
        throw 'Computer B kit source checkout changed while the immutable kit was being assembled'
    }
    Move-Item -LiteralPath $stagingDirectory -Destination $finalDirectory
    $stagingDirectory = ''
    $kitVerifier = Join-Path $PSScriptRoot 'Test-PBRemoteVisualStep21ComputerBKit.ps1'
    [void](Invoke-StrictJsonTool -Script $kitVerifier -Arguments @{
        KitDirectory = $finalDirectory
        ExpectedManifestSha256 = [string]$manifestIdentity.sha256
    } -Name 'Computer B kit directory verification')

    [System.IO.Compression.ZipFile]::CreateFromDirectory($finalDirectory, $temporaryArchivePath,
        [System.IO.Compression.CompressionLevel]::Optimal, $true)
    Move-Item -LiteralPath $temporaryArchivePath -Destination $archivePath
    $archiveIdentity = Get-PBFileIdentity -Path $archivePath
    if ([UInt64]$archiveIdentity.size -eq 0 -or [UInt64]$archiveIdentity.size -gt 2GB)
    {
        throw 'Computer B kit archive is empty or exceeds 2 GiB'
    }
    $seal = [ordered]@{
        schema = 'PixelBridge.RemoteVisualStep21ComputerBKitSeal.1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        status = 'PREDEPLOYMENT_ONLY'
        kitName = $kitName
        headCommit = $ExpectedHeadCommit
        manifest = [ordered]@{
            path = "$kitName/COMPUTER-B-KIT-MANIFEST.json"
            size = [UInt64]$manifestIdentity.size
            sha256 = [string]$manifestIdentity.sha256
        }
        archive = [ordered]@{
            path = [System.IO.Path]::GetFileName($archivePath)
            size = [UInt64]$archiveIdentity.size
            sha256 = [string]$archiveIdentity.sha256
        }
        fileCount = [UInt32]$immutableFiles.Count
        payloadFingerprintSha256 = $payloadFingerprint
        formalStep21Accepted = $false
        certifiedRemoteVisualProfile = $false
    }
    $sealIdentity = Write-PBCreateOnlyJson -Path $sealPath -Value $seal -Depth 12
    [void](Invoke-StrictJsonTool -Script $kitVerifier -Arguments @{
        KitDirectory = $finalDirectory
        KitSealPath = $sealPath
        ArchivePath = $archivePath
        ExpectedManifestSha256 = [string]$manifestIdentity.sha256
    } -Name 'Computer B kit archive verification')
    $published = $true
    [ordered]@{
        kitDirectory = $finalDirectory
        archivePath = $archivePath
        sealPath = $sealPath
        kitName = $kitName
        headCommit = $ExpectedHeadCommit
        manifestSha256 = [string]$manifestIdentity.sha256
        archiveSha256 = [string]$archiveIdentity.sha256
        sealSha256 = [string]$sealIdentity.sha256
        packageManifestSha256 = $ExpectedPackageManifestSha256
        sourceManifestSha256 = $ExpectedSourceManifestSha256
        demoSourceSha256 = [string]$demoSource.sha256
        immutableFileCount = [UInt32]$immutableFiles.Count
        status = 'PREDEPLOYMENT_ONLY'
        executedCellCount = [UInt32]0
        formalStep21Accepted = $false
        certifiedRemoteVisualProfile = $false
    } | ConvertTo-Json -Depth 12
}
finally
{
    if (-not $published)
    {
        foreach ($path in @($stagingDirectory, $temporaryArchivePath, $archivePath, $sealPath, $finalDirectory))
        {
            if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path -LiteralPath $path))
            {
                continue
            }
            if (Test-Path -LiteralPath $path -PathType Container)
            {
                [System.IO.Directory]::Delete([System.IO.Path]::GetFullPath($path), $true)
            }
            else
            {
                [System.IO.File]::Delete([System.IO.Path]::GetFullPath($path))
            }
        }
    }
}

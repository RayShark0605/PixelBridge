#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KitDirectory,

    [string]$KitSealPath,

    [string]$ArchivePath,

    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ExpectedManifestSha256,

    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$commonModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $commonModule -Force -ErrorAction Stop

function Assert-ExactKeys
{
    param(
        [Parameter(Mandatory = $true)][object]$Dictionary,
        [Parameter(Mandatory = $true)][string[]]$ExpectedKeys,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Dictionary -isnot [System.Collections.IDictionary])
    {
        throw "$Name must be a JSON object"
    }
    $actualKeys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($key in $Dictionary.Keys)
    {
        if (-not $actualKeys.Add([string]$key))
        {
            throw "$Name contains a duplicate key"
        }
    }
    if ($actualKeys.Count -ne $ExpectedKeys.Count)
    {
        throw "$Name does not contain the exact required key set"
    }
    foreach ($key in $ExpectedKeys)
    {
        if (-not $actualKeys.Remove($key))
        {
            throw "$Name is missing exact key '$key'"
        }
    }
}

function Get-NonNegativeUInt64
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Value -or $Value -is [bool] -or $Value -isnot [ValueType])
    {
        throw "$Name must be a non-negative integer"
    }
    try
    {
        $number = [decimal]$Value
        if ($number -lt 0 -or $number -gt [decimal][UInt64]::MaxValue -or [decimal]::Truncate($number) -ne $number)
        {
            throw 'out of range or fractional'
        }
        return [UInt64]$number
    }
    catch
    {
        throw "$Name must be a non-negative integer"
    }
}

function Test-SafeRelativePath
{
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.Length -gt 1024 -or $Path.Contains('\') -or
        $Path.StartsWith('/') -or $Path.Contains(':') -or $Path.Contains("`0") -or
        $Path.Contains("`r") -or $Path.Contains("`n"))
    {
        return $false
    }
    foreach ($component in $Path.Split('/'))
    {
        if ([string]::IsNullOrWhiteSpace($component) -or $component -in @('.', '..'))
        {
            return $false
        }
    }
    return $true
}

function Test-SafeBatchComponent
{
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -and -not $Value.EndsWith('.')
}

function Get-BoundedRegularFileIdentity
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][UInt64]$MaximumBytes,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        throw "$Name does not exist as a regular file"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        [UInt64]$item.Length -eq 0 -or [UInt64]$item.Length -gt $MaximumBytes)
    {
        throw "$Name is empty, oversized, or a reparse point"
    }
    return Get-PBFileIdentity -Path $item.FullName
}

function Read-BoundedLauncherText
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )
    [void](Get-BoundedRegularFileIdentity -Path $Path -MaximumBytes 128KB -Name $Name)
    return [System.IO.File]::ReadAllText($Path, [System.Text.ASCIIEncoding]::new())
}

function Assert-ContainsExactlyOnce
{
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $firstIndex = $Text.IndexOf($Expected, [StringComparison]::Ordinal)
    if ($firstIndex -lt 0 -or $Text.IndexOf($Expected, $firstIndex + 1, [StringComparison]::Ordinal) -ge 0)
    {
        throw "$Name does not contain its expected command exactly once"
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

function Get-StreamSha256
{
    param([Parameter(Mandatory = $true)][System.IO.Stream]$Stream)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($algorithm.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant()
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

function Get-ActualImmutableFiles
{
    param([Parameter(Mandatory = $true)][string]$Root)
    $rootPrefix = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $reparsePoint = Get-ChildItem -LiteralPath $Root -Force -Recurse | Where-Object {
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
    } | Select-Object -First 1
    if ($null -ne $reparsePoint)
    {
        throw "Computer B kit contains a reparse point: $($reparsePoint.FullName)"
    }
    $files = [Collections.Generic.List[object]]::new()
    $totalBytes = [UInt64]0
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
        if (-not (Test-SafeRelativePath -Path $relativePath) -or
            ($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
            [UInt64]$file.Length -eq 0 -or [UInt64]$file.Length -gt 2GB)
        {
            throw "Computer B kit contains an invalid or unbounded immutable file: $relativePath"
        }
        if ($files.Count -ge 4096 -or $totalBytes -gt [UInt64](4GB) - [UInt64]$file.Length)
        {
            throw 'Computer B kit actual immutable inventory exceeds its resource bounds'
        }
        $totalBytes += [UInt64]$file.Length
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

$resolvedKit = [System.IO.Path]::GetFullPath($KitDirectory).TrimEnd('\')
if (-not (Test-Path -LiteralPath $resolvedKit -PathType Container))
{
    throw "Computer B kit directory does not exist: $resolvedKit"
}
$kitItem = Get-Item -LiteralPath $resolvedKit
if (($kitItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
{
    throw 'Computer B kit directory must not be a reparse point'
}
$resolvedOutputPath = $null
if (-not [string]::IsNullOrWhiteSpace($OutputPath))
{
    $resolvedOutputPath = [System.IO.Path]::GetFullPath($OutputPath)
    $kitPrefix = $resolvedKit + '\'
    $workingPrefix = [System.IO.Path]::GetFullPath((Join-Path $resolvedKit 'Working')).TrimEnd('\') + '\'
    if ($resolvedOutputPath.Equals($resolvedKit, [StringComparison]::OrdinalIgnoreCase) -or
        ($resolvedOutputPath.StartsWith($kitPrefix, [StringComparison]::OrdinalIgnoreCase) -and
         -not $resolvedOutputPath.StartsWith($workingPrefix, [StringComparison]::OrdinalIgnoreCase)))
    {
        throw 'Computer B kit verification output must remain outside the immutable kit or below Working'
    }
}
$manifestPath = Join-Path $resolvedKit 'COMPUTER-B-KIT-MANIFEST.json'
$manifestIdentity = Get-BoundedRegularFileIdentity -Path $manifestPath -MaximumBytes 8MB -Name 'Computer B kit manifest'
if ((-not [string]::IsNullOrEmpty($ExpectedManifestSha256) -and
     [string]$manifestIdentity.sha256 -cne $ExpectedManifestSha256.ToLowerInvariant()))
{
    throw 'Computer B kit manifest size or expected SHA-256 is invalid'
}
$manifest = Read-PBBoundedJson -Path $manifestPath -MaximumBytes 8MB
Assert-ExactKeys -Dictionary $manifest -ExpectedKeys @('schema', 'createdUtc', 'status', 'kitName', 'headCommit',
    'headTree', 'package', 'sourceSet', 'operatorTools', 'launchers', 'truthBoundary', 'fileCount', 'files',
    'payloadFingerprintSha256') -Name 'Computer B kit manifest'
$fileCount = Get-NonNegativeUInt64 -Value $manifest.fileCount -Name 'Computer B kit fileCount'
if ([string]$manifest.schema -cne 'PixelBridge.RemoteVisualStep21ComputerBKit.1' -or
    [string]$manifest.status -cne 'PREDEPLOYMENT_ONLY' -or
    [string]$manifest.kitName -cne [System.IO.Path]::GetFileName($resolvedKit) -or
    [string]$manifest.headCommit -cnotmatch '^[0-9a-f]{40}$' -or
    [string]$manifest.headTree -cnotmatch '^[0-9a-f]{40}$' -or $fileCount -eq 0 -or $fileCount -gt 4096 -or
    @($manifest.files).Count -ne $fileCount -or
    [string]$manifest.payloadFingerprintSha256 -cnotmatch '^[0-9a-f]{64}$')
{
    throw 'Computer B kit schema, identity, or inventory count is invalid'
}

$expectedFiles = [Collections.Generic.List[object]]::new()
$seenPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$previousPath = ''
$totalBytes = [UInt64]0
foreach ($file in @($manifest.files))
{
    Assert-ExactKeys -Dictionary $file -ExpectedKeys @('path', 'size', 'sha256') -Name 'Computer B kit file identity'
    $relativePath = [string]$file.path
    $size = Get-NonNegativeUInt64 -Value $file.size -Name "Computer B kit file '$relativePath' size"
    if (-not (Test-SafeRelativePath -Path $relativePath) -or
        $relativePath -ceq 'COMPUTER-B-KIT-MANIFEST.json' -or
        $relativePath.StartsWith('Working/', [StringComparison]::OrdinalIgnoreCase) -or
        -not $seenPaths.Add($relativePath) -or [string]$file.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        ($previousPath.Length -ne 0 -and [string]::CompareOrdinal($previousPath, $relativePath) -ge 0) -or
        $size -eq 0 -or $size -gt 2GB)
    {
        throw "Computer B kit contains an invalid, duplicate, unsorted, or unbounded path: $relativePath"
    }
    if ($totalBytes -gt [UInt64](4GB) - $size)
    {
        throw 'Computer B kit immutable payload exceeds 4 GiB'
    }
    $totalBytes += $size
    $previousPath = $relativePath
    [void]$expectedFiles.Add([ordered]@{ path = $relativePath; size = $size; sha256 = [string]$file.sha256 })
}
if ((Get-CanonicalFingerprint -Files @($expectedFiles)) -cne [string]$manifest.payloadFingerprintSha256)
{
    throw 'Computer B kit manifest payload fingerprint is invalid'
}
$actualFiles = @(Get-ActualImmutableFiles -Root $resolvedKit)
if ($actualFiles.Count -ne $expectedFiles.Count)
{
    throw "Computer B kit immutable file count mismatch: actual=$($actualFiles.Count) expected=$($expectedFiles.Count)"
}
for ($index = 0; $index -lt $expectedFiles.Count; $index++)
{
    $actual = $actualFiles[$index]
    $expected = $expectedFiles[$index]
    if ([string]$actual.path -cne [string]$expected.path -or [UInt64]$actual.size -ne [UInt64]$expected.size -or
        [string]$actual.sha256 -cne [string]$expected.sha256)
    {
        throw "Computer B kit immutable file identity mismatch at '$($expected.path)'"
    }
}

Assert-ExactKeys -Dictionary $manifest.package -ExpectedKeys @('directoryName', 'sealFileName', 'manifestSha256',
    'payloadFingerprintSha256', 'endpointRoles') -Name 'Computer B kit package binding'
$packageDirectoryName = [string]$manifest.package.directoryName
$packageSealFileName = [string]$manifest.package.sealFileName
if (-not (Test-SafeBatchComponent -Value $packageDirectoryName) -or
    -not (Test-SafeBatchComponent -Value $packageSealFileName) -or
    $packageSealFileName -cne "$packageDirectoryName.seal.json" -or
    [string]$manifest.package.manifestSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    [string]$manifest.package.payloadFingerprintSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    @($manifest.package.endpointRoles).Count -ne 2 -or
    [string]$manifest.package.endpointRoles[0] -cne 'Encoder' -or
    [string]$manifest.package.endpointRoles[1] -cne 'Decoder')
{
    throw 'Computer B kit portable-package binding is invalid'
}
$packageDirectory = Join-Path $resolvedKit $packageDirectoryName
$packageSeal = Join-Path $resolvedKit $packageSealFileName
$packageVerifier = Join-Path $resolvedKit 'operator-tools\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualPortablePackage.ps1'
$packageVerification = Invoke-StrictJsonTool -Script $packageVerifier -Arguments @{
    PackageDirectory = $packageDirectory
    PackageSealPath = $packageSeal
    ExpectedManifestSha256 = [string]$manifest.package.manifestSha256
} -Name 'embedded portable-package verification'
if ($packageVerification.verified -isnot [bool] -or -not [bool]$packageVerification.verified -or
    [string]$packageVerification.packagePayloadFingerprintSha256 -cne [string]$manifest.package.payloadFingerprintSha256)
{
    throw 'Computer B kit portable package did not pass the embedded authoritative verifier'
}
$packageManifest = Read-PBBoundedJson -Path (Join-Path $packageDirectory 'package-manifest.json') -MaximumBytes 32MB
if ([string]$packageManifest.buildIdentity.headCommit -cne [string]$manifest.headCommit -or
    [string]$packageManifest.buildIdentity.headTree -cne [string]$manifest.headTree)
{
    throw 'Computer B kit package source identity differs from the kit identity'
}

Assert-ExactKeys -Dictionary $manifest.sourceSet -ExpectedKeys @('directoryName', 'sealFileName', 'manifestSha256',
    'sourceSetId', 'sourceSetFingerprintSha256', 'demoSource') -Name 'Computer B kit source-set binding'
Assert-ExactKeys -Dictionary $manifest.sourceSet.demoSource -ExpectedKeys @('path', 'size', 'sha256') `
    -Name 'Computer B kit demo source binding'
$sourceSetDirectoryName = [string]$manifest.sourceSet.directoryName
$sourceSetSealFileName = [string]$manifest.sourceSet.sealFileName
if (-not (Test-SafeBatchComponent -Value $sourceSetDirectoryName) -or
    -not (Test-SafeBatchComponent -Value $sourceSetSealFileName) -or
    $sourceSetSealFileName -cne "$sourceSetDirectoryName.seal.json" -or
    $sourceSetDirectoryName -ieq $packageDirectoryName -or
    [string]$manifest.sourceSet.manifestSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    [string]$manifest.sourceSet.sourceSetId -cnotmatch '^[0-9a-f]{32}$' -or
    [string]$manifest.sourceSet.sourceSetFingerprintSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    [string]$manifest.sourceSet.demoSource.path -cne 'random-1MiB.bin' -or
    (Get-NonNegativeUInt64 -Value $manifest.sourceSet.demoSource.size -Name 'Computer B kit demo source size') -ne 1MB -or
    [string]$manifest.sourceSet.demoSource.sha256 -cnotmatch '^[0-9a-f]{64}$')
{
    throw 'Computer B kit source-set or demo-source binding is invalid'
}
$sourceSetDirectory = Join-Path $resolvedKit $sourceSetDirectoryName
$sourceSetSeal = Join-Path $resolvedKit $sourceSetSealFileName
$sourceSetVerifier = Join-Path $resolvedKit 'operator-tools\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualSourceSet.ps1'
$sourceSetVerification = Invoke-StrictJsonTool -Script $sourceSetVerifier -Arguments @{
    SourceSetDirectory = $sourceSetDirectory
    SourceSealPath = $sourceSetSeal
    ExpectedManifestSha256 = [string]$manifest.sourceSet.manifestSha256
} -Name 'embedded source-set verification'
if ($sourceSetVerification.verified -isnot [bool] -or -not [bool]$sourceSetVerification.verified -or
    [string]$sourceSetVerification.sourceSetId -cne [string]$manifest.sourceSet.sourceSetId -or
    [string]$sourceSetVerification.sourceSetFingerprintSha256 -cne [string]$manifest.sourceSet.sourceSetFingerprintSha256)
{
    throw 'Computer B kit source set did not pass the embedded authoritative verifier'
}
$demoSourceIdentity = Get-PBFileIdentity -Path (Join-Path $sourceSetDirectory 'random-1MiB.bin')
if ([UInt64]$demoSourceIdentity.size -ne [UInt64]$manifest.sourceSet.demoSource.size -or
    [string]$demoSourceIdentity.sha256 -cne [string]$manifest.sourceSet.demoSource.sha256)
{
    throw 'Computer B kit demo source identity is invalid'
}

Assert-ExactKeys -Dictionary $manifest.operatorTools -ExpectedKeys @('root', 'fileCount', 'fingerprintSha256') `
    -Name 'Computer B kit operator-tools binding'
$operatorToolCount = Get-NonNegativeUInt64 -Value $manifest.operatorTools.fileCount `
    -Name 'Computer B kit operator-tools fileCount'
$operatorToolFiles = @($expectedFiles | Where-Object { [string]$_.path -clike 'operator-tools/*' })
if ([string]$manifest.operatorTools.root -cne 'operator-tools' -or $operatorToolCount -eq 0 -or
    $operatorToolCount -ne $operatorToolFiles.Count -or
    [string]$manifest.operatorTools.fingerprintSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    (Get-CanonicalFingerprint -Files $operatorToolFiles) -cne [string]$manifest.operatorTools.fingerprintSha256)
{
    throw 'Computer B kit operator-tools inventory is invalid'
}

Assert-ExactKeys -Dictionary $manifest.launchers -ExpectedKeys @('verify', 'captureMonitorCatalog',
    'experimentMonitorDemo', 'demoProfile', 'demoLogicalFps', 'demoControlRepetitions',
    'demoMonitorSelector', 'computerBProtectedMonitor', 'computerBExperimentMonitor') -Name 'Computer B kit launchers'
$demoLogicalFps = Get-NonNegativeUInt64 -Value $manifest.launchers.demoLogicalFps -Name 'Computer B kit demo logical FPS'
$demoControlRepetitions = Get-NonNegativeUInt64 -Value $manifest.launchers.demoControlRepetitions `
    -Name 'Computer B kit demo control repetitions'
$protectedMonitorDeviceName = [string]$manifest.launchers.computerBProtectedMonitor
$experimentMonitorDeviceName = [string]$manifest.launchers.computerBExperimentMonitor
if ([string]$manifest.launchers.verify -cne 'VERIFY-COMPUTER-B-KIT.bat' -or
    [string]$manifest.launchers.captureMonitorCatalog -cne 'Capture-ComputerBMonitorCatalog.bat' -or
    [string]$manifest.launchers.experimentMonitorDemo -cne 'Start-PBRemoteVisualExperimentMonitorDemo.bat' -or
    [string]$manifest.launchers.demoProfile -cne 'remote-lf4' -or $demoLogicalFps -lt 1 -or $demoLogicalFps -gt 5 -or
    $demoControlRepetitions -lt 1 -or $demoControlRepetitions -gt 64 -or
    [string]$manifest.launchers.demoMonitorSelector -cne 'primary' -or
    $protectedMonitorDeviceName -cnotmatch '^\\\\\.\\DISPLAY[1-9][0-9]*$' -or
    $experimentMonitorDeviceName -cnotmatch '^\\\\\.\\DISPLAY[1-9][0-9]*$' -or
    $protectedMonitorDeviceName -ieq $experimentMonitorDeviceName)
{
    throw 'Computer B kit launcher contract is invalid'
}
$verifyLauncherPath = Join-Path $resolvedKit ([string]$manifest.launchers.verify)
$captureLauncherPath = Join-Path $resolvedKit ([string]$manifest.launchers.captureMonitorCatalog)
$demoLauncherPath = Join-Path $resolvedKit ([string]$manifest.launchers.experimentMonitorDemo)
$verifyLauncherText = Read-BoundedLauncherText -Path $verifyLauncherPath -Name 'Computer B kit verifier launcher'
$captureLauncherText = Read-BoundedLauncherText -Path $captureLauncherPath -Name 'Computer B kit catalog launcher'
$demoLauncherText = Read-BoundedLauncherText -Path $demoLauncherPath -Name 'Computer B kit demo launcher'
Assert-ContainsExactlyOnce -Text $verifyLauncherText `
    -Expected 'operator-tools\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualStep21ComputerBKit.ps1' `
    -Name 'Computer B kit verifier launcher'
Assert-ContainsExactlyOnce -Text $captureLauncherText `
    -Expected ('set "DECODER=%~dp0' + $packageDirectoryName + '\Decoder\PixelBridgeDecoder.exe"') `
    -Name 'Computer B kit catalog launcher Decoder binding'
Assert-ContainsExactlyOnce -Text $captureLauncherText -Expected '"%DECODER%" --list-monitors > "%PARTIAL%"' `
    -Name 'Computer B kit catalog launcher command'
Assert-ContainsExactlyOnce -Text $captureLauncherText -Expected 'if exist "%PARTIAL%" (' `
    -Name 'Computer B kit catalog launcher create-only partial guard'
Assert-ContainsExactlyOnce -Text $demoLauncherText `
    -Expected ('set "ENCODER=%~dp0' + $packageDirectoryName + '\Encoder\PixelBridgeEncoder.exe"') `
    -Name 'Computer B kit demo launcher Encoder binding'
Assert-ContainsExactlyOnce -Text $demoLauncherText `
    -Expected ('set "SOURCE=%~dp0' + $sourceSetDirectoryName + '\random-1MiB.bin"') `
    -Name 'Computer B kit demo launcher source binding'
Assert-ContainsExactlyOnce -Text $demoLauncherText `
    -Expected ('set "EXPECTED_SHA256=' + [string]$manifest.sourceSet.demoSource.sha256 + '"') `
    -Name 'Computer B kit demo launcher source-hash binding'
Assert-ContainsExactlyOnce -Text $demoLauncherText `
    -Expected 'set "CERTUTIL=%SystemRoot%\System32\certutil.exe"' `
    -Name 'Computer B kit demo launcher SHA-256 tool binding'
Assert-ContainsExactlyOnce -Text $demoLauncherText `
    -Expected 'for /f "skip=1 tokens=*" %%H in (''%CERTUTIL% -hashfile "%SOURCE%" SHA256'') do if not defined ACTUAL_SHA256 set "ACTUAL_SHA256=%%H"' `
    -Name 'Computer B kit demo launcher SHA-256 command'
$expectedDemoCommand = '"%ENCODER%" --headless-broadcast --source "%SOURCE%" --profile remote-lf4 --channel remote --remote-provider UserProvidedVisualLink --compression off --single-monitor-fullscreen primary --logical-fps ' +
    $demoLogicalFps + ' --control-repetitions ' +
    $demoControlRepetitions + ' --manual-stop --loop --run-id "%RUN_ID%" --journal "%RUN_DIR%\encoder-journal.ndjson" --report "%RUN_DIR%\encoder-report.json"'
Assert-ContainsExactlyOnce -Text $demoLauncherText -Expected $expectedDemoCommand -Name 'Computer B kit demo launcher command'
if ($demoLauncherText.Contains('--protected-monitor', [StringComparison]::Ordinal) -or
    $demoLauncherText.Contains('--origin', [StringComparison]::Ordinal) -or
    -not $demoLauncherText.Contains('if /i "%PB_PACKAGE_PREFLIGHT_ONLY%"=="1" (', [StringComparison]::Ordinal))
{
    throw 'Computer B kit demo launcher contains incompatible formal-monitor arguments or lacks preflight'
}

Assert-ExactKeys -Dictionary $manifest.truthBoundary -ExpectedKeys @('formalStep21Accepted', 'executedCellCount',
    'certifiedRemoteVisualProfile', 'demoLauncherIsFormalCell', 'providerUiEvidenceStillRequiredPerRun',
    'freshRunIdAndPilotPlanStillRequiredPerCell', 'statement') -Name 'Computer B kit truth boundary'
$executedCellCount = Get-NonNegativeUInt64 -Value $manifest.truthBoundary.executedCellCount `
    -Name 'Computer B kit executedCellCount'
$truthStatement = 'This is a predeployment Computer B kit. It allocates no formal RunId, executes no Step 21 cell, and does not certify a RemoteVisual profile.'
if ($manifest.truthBoundary.formalStep21Accepted -isnot [bool] -or [bool]$manifest.truthBoundary.formalStep21Accepted -or
    $executedCellCount -ne 0 -or $manifest.truthBoundary.certifiedRemoteVisualProfile -isnot [bool] -or
    [bool]$manifest.truthBoundary.certifiedRemoteVisualProfile -or
    $manifest.truthBoundary.demoLauncherIsFormalCell -isnot [bool] -or [bool]$manifest.truthBoundary.demoLauncherIsFormalCell -or
    $manifest.truthBoundary.providerUiEvidenceStillRequiredPerRun -isnot [bool] -or
    -not [bool]$manifest.truthBoundary.providerUiEvidenceStillRequiredPerRun -or
    $manifest.truthBoundary.freshRunIdAndPilotPlanStillRequiredPerCell -isnot [bool] -or
    -not [bool]$manifest.truthBoundary.freshRunIdAndPilotPlanStillRequiredPerCell -or
    [string]$manifest.truthBoundary.statement -cne $truthStatement)
{
    throw 'Computer B kit truth boundary is invalid'
}

$seal = $null
$resolvedSeal = $null
if (-not [string]::IsNullOrWhiteSpace($KitSealPath))
{
    $resolvedSeal = [System.IO.Path]::GetFullPath($KitSealPath)
    $seal = Read-PBBoundedJson -Path $resolvedSeal -MaximumBytes 1MB
    Assert-ExactKeys -Dictionary $seal -ExpectedKeys @('schema', 'createdUtc', 'status', 'kitName', 'headCommit',
        'manifest', 'archive', 'fileCount', 'payloadFingerprintSha256', 'formalStep21Accepted',
        'certifiedRemoteVisualProfile') -Name 'Computer B kit seal'
    Assert-ExactKeys -Dictionary $seal.manifest -ExpectedKeys @('path', 'size', 'sha256') -Name 'Computer B kit sealed manifest'
    Assert-ExactKeys -Dictionary $seal.archive -ExpectedKeys @('path', 'size', 'sha256') -Name 'Computer B kit sealed archive'
    if ([string]$seal.schema -cne 'PixelBridge.RemoteVisualStep21ComputerBKitSeal.1' -or
        [string]$seal.status -cne 'PREDEPLOYMENT_ONLY' -or [string]$seal.kitName -cne [string]$manifest.kitName -or
        [string]$seal.headCommit -cne [string]$manifest.headCommit -or
        [string]$seal.manifest.path -cne "$($manifest.kitName)/COMPUTER-B-KIT-MANIFEST.json" -or
        [UInt64](Get-NonNegativeUInt64 -Value $seal.manifest.size -Name 'Computer B kit sealed manifest size') -ne [UInt64]$manifestIdentity.size -or
        [string]$seal.manifest.sha256 -cne [string]$manifestIdentity.sha256 -or
        [UInt64](Get-NonNegativeUInt64 -Value $seal.fileCount -Name 'Computer B kit sealed fileCount') -ne $fileCount -or
        [string]$seal.payloadFingerprintSha256 -cne [string]$manifest.payloadFingerprintSha256 -or
        $seal.formalStep21Accepted -isnot [bool] -or [bool]$seal.formalStep21Accepted -or
        $seal.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$seal.certifiedRemoteVisualProfile)
    {
        throw 'Computer B kit seal identity or truth boundary is invalid'
    }
}
elseif (-not [string]::IsNullOrWhiteSpace($ArchivePath))
{
    throw 'Computer B kit archive verification requires KitSealPath'
}

$resolvedArchive = $null
if (-not [string]::IsNullOrWhiteSpace($ArchivePath))
{
    $resolvedArchive = [System.IO.Path]::GetFullPath($ArchivePath)
    $archiveIdentity = Get-BoundedRegularFileIdentity -Path $resolvedArchive -MaximumBytes 2GB -Name 'Computer B kit archive'
    if ([string]$seal.archive.path -cne [System.IO.Path]::GetFileName($resolvedArchive) -or
        [UInt64](Get-NonNegativeUInt64 -Value $seal.archive.size -Name 'Computer B kit sealed archive size') -ne [UInt64]$archiveIdentity.size -or
        [string]$seal.archive.sha256 -cne [string]$archiveIdentity.sha256)
    {
        throw 'Computer B kit archive identity differs from its seal'
    }
    $expectedArchiveFiles = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    $archiveManifestPath = "$($manifest.kitName)/COMPUTER-B-KIT-MANIFEST.json"
    $expectedArchiveFiles.Add($archiveManifestPath, [ordered]@{ size = [UInt64]$manifestIdentity.size; sha256 = [string]$manifestIdentity.sha256 })
    foreach ($file in $expectedFiles)
    {
        $expectedArchiveFiles.Add("$($manifest.kitName)/$($file.path)", $file)
    }
    $seenArchiveFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedArchive)
    try
    {
        if ($archive.Entries.Count -ne $expectedArchiveFiles.Count)
        {
            throw 'Computer B kit archive entry count is invalid'
        }
        foreach ($entry in $archive.Entries)
        {
            $entryPath = $entry.FullName.Replace('\', '/')
            if ($entryPath.EndsWith('/') -or -not (Test-SafeRelativePath -Path $entryPath) -or
                -not $seenArchiveFiles.Add($entryPath) -or -not $expectedArchiveFiles.ContainsKey($entryPath))
            {
                throw "Computer B kit archive contains an invalid, duplicate, or unmanifested entry: $entryPath"
            }
            $expected = $expectedArchiveFiles[$entryPath]
            if ([UInt64]$entry.Length -ne [UInt64]$expected.size)
            {
                throw "Computer B kit archive entry size mismatch: $entryPath"
            }
            $entryStream = $entry.Open()
            try
            {
                $entrySha256 = Get-StreamSha256 -Stream $entryStream
            }
            finally
            {
                $entryStream.Dispose()
            }
            if ($entrySha256 -cne [string]$expected.sha256)
            {
                throw "Computer B kit archive entry hash mismatch: $entryPath"
            }
        }
    }
    finally
    {
        $archive.Dispose()
    }
}

$result = [ordered]@{
    schema = 'PixelBridge.RemoteVisualStep21ComputerBKitVerification.1'
    verified = $true
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    status = 'PREDEPLOYMENT_ONLY'
    kitDirectory = $resolvedKit
    manifest = $manifestIdentity
    kitSealPath = $resolvedSeal
    archivePath = $resolvedArchive
    headCommit = [string]$manifest.headCommit
    packageManifestSha256 = [string]$manifest.package.manifestSha256
    sourceSetId = [string]$manifest.sourceSet.sourceSetId
    demoSource = $manifest.sourceSet.demoSource
    immutableFileCount = [UInt32]$fileCount
    immutablePayloadBytes = $totalBytes
    payloadFingerprintSha256 = [string]$manifest.payloadFingerprintSha256
    formalStep21Accepted = $false
    executedCellCount = [UInt32]0
    certifiedRemoteVisualProfile = $false
}
if ($null -ne $resolvedOutputPath)
{
    [void](Write-PBCreateOnlyJson -Path $resolvedOutputPath -Value $result -Depth 15)
}
$result | ConvertTo-Json -Depth 15

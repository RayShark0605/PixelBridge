[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [string]$PackageSealPath,

    [string]$ArchivePath,

    [string]$ExpectedManifestSha256,

    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Write-NewUtf8File
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
        try
        {
            $writer.Write($Content)
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

function Get-FileSha256
{
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
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

function Read-BoundedJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [UInt64]$MaximumBytes = 32MB
    )
    $item = Get-Item -LiteralPath $Path
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or $item.Length -eq 0 -or
        [UInt64]$item.Length -gt $MaximumBytes)
    {
        throw "JSON artifact is empty, oversized, or a reparse point: $Path"
    }
    try
    {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable -Depth 100
    }
    catch
    {
        throw "JSON artifact is malformed: $Path ($($_.Exception.Message))"
    }
}

function Require-Hash
{
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Value -isnot [string] -or $Value -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "$Name must be 64 lowercase hexadecimal characters"
    }
}

function Get-ApplicationBuildIdentity
{
    param(
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [Parameter(Mandatory = $true)][string]$ExpectedApplicationName,
        [Parameter(Mandatory = $true)][string]$ExpectedGitCommit
    )
    $output = @(& $ExecutablePath --build-identity 2>&1)
    if ($LASTEXITCODE -ne 0)
    {
        throw "$ExpectedApplicationName --build-identity failed with exit code $LASTEXITCODE"
    }
    $text = ($output -join "`n").Trim()
    if ([string]::IsNullOrWhiteSpace($text) -or [System.Text.UTF8Encoding]::new($false).GetByteCount($text) -gt 16KB)
    {
        throw "$ExpectedApplicationName --build-identity returned an empty or oversized result"
    }
    try
    {
        $identity = $text | ConvertFrom-Json -AsHashtable -Depth 8
    }
    catch
    {
        throw "$ExpectedApplicationName --build-identity did not return valid JSON"
    }
    $expectedKeys = @('schema', 'applicationName', 'applicationVersion', 'protocolMajor', 'protocolMinor', 'gitCommit')
    if ($identity -isnot [System.Collections.IDictionary] -or $identity.Count -ne $expectedKeys.Count)
    {
        throw "$ExpectedApplicationName --build-identity has an invalid object shape"
    }
    foreach ($key in $expectedKeys)
    {
        if (-not $identity.Contains($key))
        {
            throw "$ExpectedApplicationName --build-identity is missing $key"
        }
    }
    if ($identity.schema -isnot [string] -or $identity.schema -cne 'PixelBridge.ApplicationBuildIdentity.1' -or
        $identity.applicationName -isnot [string] -or $identity.applicationName -cne $ExpectedApplicationName -or
        $identity.applicationVersion -isnot [string] -or [string]::IsNullOrWhiteSpace($identity.applicationVersion) -or
        $identity.protocolMajor -isnot [Int64] -or $identity.protocolMajor -ne 1 -or
        $identity.protocolMinor -isnot [Int64] -or $identity.protocolMinor -ne 0 -or
        $identity.gitCommit -isnot [string] -or $identity.gitCommit -cne $ExpectedGitCommit)
    {
        throw "$ExpectedApplicationName runtime build identity does not match package HEAD/protocol"
    }
    return $identity
}

function Test-RelativePath
{
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.Length -gt 1024 -or $Path.Contains('\') -or
        $Path.StartsWith('/') -or $Path.Contains(':') -or $Path.Contains("`0"))
    {
        return $false
    }
    foreach ($component in $Path.Split('/'))
    {
        if ([string]::IsNullOrWhiteSpace($component) -or $component -eq '.' -or $component -eq '..')
        {
            return $false
        }
    }
    return $true
}

function Get-CanonicalInventoryFingerprint
{
    param([Parameter(Mandatory = $true)][object[]]$Inventory)
    $sortedInventory = @($Inventory | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
    $canonical = ($sortedInventory | ForEach-Object { "$($_.path)`0$($_.size)`0$($_.sha256)`n" }) -join ''
    return Get-TextSha256 -Text $canonical
}

function Get-ActualInventory
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )
    $resolvedRootPrefix = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $directoryReparse = Get-ChildItem -LiteralPath $Root -Directory -Recurse -Force | Where-Object {
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
    } | Select-Object -First 1
    if ($null -ne $directoryReparse)
    {
        throw "Package contains a directory reparse point: $($directoryReparse.FullName)"
    }
    $manifestFullPath = [System.IO.Path]::GetFullPath($ManifestPath)
    $entries = @()
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse -Force)
    {
        $fullPath = [System.IO.Path]::GetFullPath($file.FullName)
        if ($fullPath -ceq $manifestFullPath)
        {
            continue
        }
        if (-not $fullPath.StartsWith($resolvedRootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            ($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "Package file escaped the root or is a reparse point: $fullPath"
        }
        $entries += [ordered]@{
            path = $fullPath.Substring($resolvedRootPrefix.Length).Replace('\', '/')
            size = [UInt64]$file.Length
            sha256 = Get-FileSha256 -Path $fullPath
        }
    }
    return @($entries | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
}

$resolvedPackage = [System.IO.Path]::GetFullPath($PackageDirectory)
if (-not (Test-Path -LiteralPath $resolvedPackage -PathType Container))
{
    throw "Package directory does not exist: $resolvedPackage"
}
$packageItem = Get-Item -LiteralPath $resolvedPackage
if (($packageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
{
    throw "Package directory cannot be a reparse point: $resolvedPackage"
}
$manifestPath = Join-Path $resolvedPackage 'package-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf))
{
    throw "Package manifest is missing: $manifestPath"
}
$manifest = Read-BoundedJson -Path $manifestPath
if ($manifest.schema -cne 'PixelBridge.PortablePackage.2')
{
    throw "Package manifest schema mismatch: $($manifest.schema)"
}
$manifestSha256 = Get-FileSha256 -Path $manifestPath
Require-Hash -Value $manifestSha256 -Name 'manifest SHA-256'
if ($ExpectedManifestSha256)
{
    if ($ExpectedManifestSha256 -cnotmatch '^[0-9a-fA-F]{64}$' -or
        $manifestSha256 -cne $ExpectedManifestSha256.ToLowerInvariant())
    {
        throw "Package manifest SHA-256 does not match the expected deployed identity"
    }
}
$packageName = Split-Path -Leaf $resolvedPackage
if ($manifest.packageName -cne $packageName)
{
    throw "Package directory name does not match manifest.packageName"
}

$endpointRoles = @($manifest.endpointRoles)
if ($endpointRoles.Count -lt 1 -or $endpointRoles.Count -gt 2 -or
    @($endpointRoles | Where-Object { $_ -cne 'Encoder' -and $_ -cne 'Decoder' }).Count -ne 0 -or
    @($endpointRoles | Sort-Object -Unique).Count -ne $endpointRoles.Count)
{
    throw 'endpointRoles must contain one or both unique Encoder/Decoder roles'
}
$applications = @($manifest.applications)
if ($applications.Count -ne $endpointRoles.Count)
{
    throw 'applications must match endpointRoles exactly'
}
$applicationRoleSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($application in $applications)
{
    $expectedApplicationName = "PixelBridge$($application.role)"
    $expectedExecutablePath = if ($endpointRoles.Count -eq 2) {
        "$($application.role)/$expectedApplicationName.exe"
    } else {
        "$expectedApplicationName.exe"
    }
    if (-not ($endpointRoles -ccontains $application.role) -or -not $applicationRoleSet.Add([string]$application.role) -or
        $application.application -cne $expectedApplicationName -or
        $application.relativeExecutablePath -cne $expectedExecutablePath)
    {
        throw "Application role/name/path binding is invalid: $($application.role)"
    }
}

Require-Hash -Value $manifest.buildIdentityFingerprintSha256 -Name 'buildIdentityFingerprintSha256'
Require-Hash -Value $manifest.packagePayloadFingerprintSha256 -Name 'packagePayloadFingerprintSha256'
$buildIdentity = $manifest.buildIdentity
foreach ($name in @('headCommit', 'headTree', 'phase1GatePassTagObject', 'phase1GatePassCommit'))
{
    $value = $buildIdentity[$name]
    if ($value -isnot [string] -or $value -cnotmatch '^[0-9a-f]{40}$')
    {
        throw "buildIdentity.$name must be 40 lowercase hexadecimal characters"
    }
}
Require-Hash -Value $buildIdentity.testedSourceFingerprintSha256 -Name 'testedSourceFingerprintSha256'
if ($buildIdentity.configuration -cne 'Release' -or $buildIdentity.remoteVisualLowFpsProfile.token -cne 'remote-lf4' -or
    [UInt64]$buildIdentity.remoteVisualLowFpsProfile.visualProfileId -ne [UInt64]5783275402097472561 -or
    [UInt32]$buildIdentity.remoteVisualLowFpsProfile.layoutVersion -ne 7 -or
    [UInt32]$buildIdentity.remoteVisualLowFpsProfile.codedDataBytesPerFrame -ne 8100 -or
    [UInt32]$buildIdentity.remoteVisualLowFpsProfile.codewordsPerFrame -ne 4 -or
    [UInt32]$buildIdentity.remoteVisualLowFpsProfile.logicalVisualFpsMinimum -ne 1 -or
    [UInt32]$buildIdentity.remoteVisualLowFpsProfile.logicalVisualFpsMaximum -ne 5)
{
    throw 'Package has an incompatible LF4 build/profile identity'
}
if ($buildIdentity.compiler.id -cne 'MSVC' -or [string]::IsNullOrWhiteSpace($buildIdentity.compiler.version) -or
    $buildIdentity.compiler.architecture -cne 'x64' -or [string]::IsNullOrWhiteSpace($buildIdentity.windowsSdkVersion) -or
    [string]::IsNullOrWhiteSpace($buildIdentity.qt.version) -or
    $buildIdentity.vcpkg.builtinBaseline -cnotmatch '^[0-9a-f]{40}$' -or
    $buildIdentity.vcpkg.targetTriplet -cne 'x64-windows')
{
    throw 'Package toolchain/Qt/vcpkg identity is incomplete or unsupported'
}
foreach ($hashValue in @($buildIdentity.cmake.cacheSha256, $buildIdentity.compiler.sha256,
    $buildIdentity.qt.deployedQt6CoreSha256, $buildIdentity.qt.licenseInfoSha256,
    $buildIdentity.vcpkg.manifestSha256, $buildIdentity.vcpkg.installedStatusSha256))
{
    Require-Hash -Value $hashValue -Name 'build identity artifact SHA-256'
}
if ($null -ne $buildIdentity.vcpkg.configurationSha256)
{
    Require-Hash -Value $buildIdentity.vcpkg.configurationSha256 -Name 'vcpkg configuration SHA-256'
}
$vcpkgPackages = @($buildIdentity.vcpkg.packages)
if ($vcpkgPackages.Count -lt 5 -or $vcpkgPackages.Count -gt 256)
{
    throw 'vcpkg package inventory is outside 5..256'
}
foreach ($requiredPackage in @('blake3', 'libpng', 'wirehair', 'zlib', 'zstd'))
{
    if (-not ($vcpkgPackages.name -ccontains $requiredPackage))
    {
        throw "Required runtime dependency is absent from build identity: $requiredPackage"
    }
}
foreach ($package in $vcpkgPackages)
{
    if ([string]::IsNullOrWhiteSpace([string]$package.name) -or
        [string]::IsNullOrWhiteSpace([string]$package.version) -or
        $package.architecture -cne 'x64-windows' -or $package.abi -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "vcpkg package identity is incomplete: $($package.name)"
    }
}
$computedBuildIdentityFingerprint = Get-TextSha256 -Text ($buildIdentity | ConvertTo-Json -Depth 16 -Compress)
if ($computedBuildIdentityFingerprint -cne $manifest.buildIdentityFingerprintSha256)
{
    throw 'buildIdentityFingerprintSha256 does not match the embedded build identity'
}

$sourceFiles = @($manifest.sourceFiles)
if ([UInt64]$manifest.sourceFileCount -ne [UInt64]$sourceFiles.Count -or $sourceFiles.Count -lt 1 -or
    $sourceFiles.Count -gt 65536)
{
    throw 'Source file inventory count is invalid'
}
$sourcePathSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $sourceFiles)
{
    if (-not (Test-RelativePath -Path ([string]$entry.path)) -or -not $sourcePathSet.Add([string]$entry.path) -or
        [UInt64]$entry.size -gt 16GB)
    {
        throw "Invalid or duplicate source inventory path: $($entry.path)"
    }
    Require-Hash -Value $entry.sha256 -Name "source hash $($entry.path)"
}
$sortedSourceFiles = @($sourceFiles | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
if ((Get-CanonicalInventoryFingerprint -Inventory $sortedSourceFiles) -cne $buildIdentity.testedSourceFingerprintSha256)
{
    throw 'Source inventory does not match testedSourceFingerprintSha256'
}

$manifestFiles = @($manifest.files)
if ([UInt64]$manifest.packageFileCount -ne [UInt64]$manifestFiles.Count -or $manifestFiles.Count -lt 1 -or
    $manifestFiles.Count -gt 4096)
{
    throw 'Package file inventory count is invalid'
}
$manifestFileMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
$manifestPayloadBytes = [UInt64]0
foreach ($entry in $manifestFiles)
{
    $path = [string]$entry.path
    if (-not (Test-RelativePath -Path $path) -or $path -ieq 'package-manifest.json' -or
        $manifestFileMap.ContainsKey($path) -or [UInt64]$entry.size -gt 2GB)
    {
        throw "Invalid or duplicate package inventory path: $path"
    }
    Require-Hash -Value $entry.sha256 -Name "package hash $path"
    $manifestFileMap.Add($path, $entry)
    $manifestPayloadBytes += [UInt64]$entry.size
    if ($manifestPayloadBytes -gt 2GB)
    {
        throw 'Package payload exceeds 2 GiB'
    }
}
$sortedManifestFiles = @($manifestFiles | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
if ([UInt64]$manifest.packagePayloadBytes -ne $manifestPayloadBytes -or
    (Get-CanonicalInventoryFingerprint -Inventory $sortedManifestFiles) -cne $manifest.packagePayloadFingerprintSha256)
{
    throw 'Package payload count/fingerprint does not match its manifest'
}
$actualFiles = @(Get-ActualInventory -Root $resolvedPackage -ManifestPath $manifestPath)
if ($actualFiles.Count -ne $manifestFiles.Count)
{
    throw "Package directory file count differs from manifest: actual=$($actualFiles.Count) manifest=$($manifestFiles.Count)"
}
foreach ($actual in $actualFiles)
{
    if (-not $manifestFileMap.ContainsKey($actual.path))
    {
        throw "Unmanifested package file: $($actual.path)"
    }
    $expected = $manifestFileMap[$actual.path]
    if ([UInt64]$expected.size -ne [UInt64]$actual.size -or $expected.sha256 -cne $actual.sha256)
    {
        throw "Package file identity mismatch: $($actual.path)"
    }
}

foreach ($application in $applications)
{
    if (-not ($endpointRoles -ccontains $application.role) -or
        -not (Test-RelativePath -Path ([string]$application.relativeExecutablePath)) -or
        -not $manifestFileMap.ContainsKey([string]$application.relativeExecutablePath))
    {
        throw "Application binding is invalid: $($application.application)"
    }
    $file = $manifestFileMap[[string]$application.relativeExecutablePath]
    if ([UInt64]$file.size -ne [UInt64]$application.size -or $file.sha256 -cne $application.sha256 -or
        [string]::IsNullOrWhiteSpace([string]$application.versionOutput))
    {
        throw "Application executable binding does not match package inventory: $($application.application)"
    }
    $executablePath = Join-Path $resolvedPackage (([string]$application.relativeExecutablePath).Replace('/', '\'))
    $runtimeIdentity = Get-ApplicationBuildIdentity -ExecutablePath $executablePath `
        -ExpectedApplicationName ([string]$application.application) -ExpectedGitCommit ([string]$buildIdentity.headCommit)
    $expectedVersionOutput = "$($application.application) $($runtimeIdentity.applicationVersion) (protocol $($runtimeIdentity.protocolMajor).$($runtimeIdentity.protocolMinor))"
    if ([string]$application.versionOutput -cne $expectedVersionOutput)
    {
        throw "Application version output disagrees with runtime build identity: $($application.application)"
    }
    $qtCorePath = if ($endpointRoles.Count -eq 2) { "$($application.role)/Qt6Core.dll" } else { 'Qt6Core.dll' }
    if (-not $manifestFileMap.ContainsKey($qtCorePath) -or
        $manifestFileMap[$qtCorePath].sha256 -cne $buildIdentity.qt.deployedQt6CoreSha256)
    {
        throw "Deployed Qt6Core identity is invalid for $($application.role)"
    }
}
foreach ($requiredPackageFile in @('THIRD_PARTY_NOTICES.txt', 'SBOM.spdx.json', 'licenses/qt/LICENSE.txt',
    'licenses/qt/Copyright.txt', 'licenses/qt/licenseInfo.txt'))
{
    if (-not $manifestFileMap.ContainsKey($requiredPackageFile))
    {
        throw "Required package metadata/license file is absent: $requiredPackageFile"
    }
}
if ($manifestFileMap['licenses/qt/licenseInfo.txt'].sha256 -cne $buildIdentity.qt.licenseInfoSha256)
{
    throw 'Packaged Qt licenseInfo identity differs from the configured Qt installation'
}
foreach ($package in $vcpkgPackages)
{
    $licensePath = "licenses/vcpkg/$($package.name).txt"
    if (-not $manifestFileMap.ContainsKey($licensePath))
    {
        throw "Installed vcpkg package has no packaged copyright notice: $($package.name)"
    }
}
$noticePath = Join-Path $resolvedPackage 'THIRD_PARTY_NOTICES.txt'
$noticeItem = Get-Item -LiteralPath $noticePath
if ([UInt64]$noticeItem.Length -eq 0 -or [UInt64]$noticeItem.Length -gt 1MB)
{
    throw 'THIRD_PARTY_NOTICES.txt is empty or oversized'
}
$noticeLines = @([System.IO.File]::ReadAllLines($noticePath, [System.Text.UTF8Encoding]::new($false)))
if (-not ($noticeLines -ccontains "Generated for package $packageName") -or
    -not ($noticeLines -ccontains "Qt $($buildIdentity.qt.version): licenses/qt/LICENSE.txt, Copyright.txt, licenseInfo.txt"))
{
    throw 'THIRD_PARTY_NOTICES.txt does not bind the package/Qt identity'
}
foreach ($package in $vcpkgPackages)
{
    $expectedNotice = "vcpkg $($package.name) $($package.version) port $($package.portVersion) ABI $($package.abi): licenses/vcpkg/$($package.name).txt"
    if (-not ($noticeLines -ccontains $expectedNotice))
    {
        throw "THIRD_PARTY_NOTICES.txt is missing the exact dependency identity: $($package.name)"
    }
}
$sbom = Read-BoundedJson -Path (Join-Path $resolvedPackage 'SBOM.spdx.json') -MaximumBytes 4MB
if ($sbom.spdxVersion -cne 'SPDX-2.3' -or $sbom.dataLicense -cne 'CC0-1.0' -or
    $sbom.SPDXID -cne 'SPDXRef-DOCUMENT' -or @($sbom.packages).Count -ne ($vcpkgPackages.Count + 2))
{
    throw 'SPDX SBOM header or package cardinality is invalid'
}
$sbomPackageMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
foreach ($package in @($sbom.packages))
{
    if ([string]::IsNullOrWhiteSpace([string]$package.name) -or $sbomPackageMap.ContainsKey([string]$package.name))
    {
        throw "SPDX SBOM has an invalid or duplicate package name: $($package.name)"
    }
    $sbomPackageMap.Add([string]$package.name, $package)
}
if (-not $sbomPackageMap.ContainsKey('PixelBridge') -or -not $sbomPackageMap.ContainsKey('Qt') -or
    $sbomPackageMap['Qt'].versionInfo -cne $buildIdentity.qt.version)
{
    throw 'SPDX SBOM is missing the PixelBridge/Qt package identity'
}
foreach ($package in $vcpkgPackages)
{
    if (-not $sbomPackageMap.ContainsKey([string]$package.name) -or
        $sbomPackageMap[[string]$package.name].versionInfo -cne $package.version)
    {
        throw "SPDX SBOM is missing the exact vcpkg package identity: $($package.name)"
    }
}

$seal = $null
$resolvedSeal = $null
if ($PackageSealPath)
{
    $resolvedSeal = [System.IO.Path]::GetFullPath($PackageSealPath)
    if (-not (Test-Path -LiteralPath $resolvedSeal -PathType Leaf))
    {
        throw "Package seal does not exist: $resolvedSeal"
    }
    $seal = Read-BoundedJson -Path $resolvedSeal -MaximumBytes 1MB
    if ($seal.schema -cne 'PixelBridge.PortablePackageSeal.2' -or $seal.packageName -cne $packageName -or
        $seal.label -cne $manifest.label -or (@($seal.endpointRoles) -join "`0") -cne ($endpointRoles -join "`0") -or
        $seal.manifest.path -cne 'package-manifest.json' -or
        [UInt64]$seal.manifest.size -ne [UInt64](Get-Item -LiteralPath $manifestPath).Length -or
        $seal.manifest.sha256 -cne $manifestSha256 -or
        $seal.buildIdentityFingerprintSha256 -cne $manifest.buildIdentityFingerprintSha256 -or
        $seal.packagePayloadFingerprintSha256 -cne $manifest.packagePayloadFingerprintSha256 -or
        $seal.testedSourceFingerprintSha256 -cne $buildIdentity.testedSourceFingerprintSha256 -or
        $seal.headCommit -cne $buildIdentity.headCommit -or $seal.headTree -cne $buildIdentity.headTree)
    {
        throw 'Package seal does not bind the exact package manifest/build/source identity'
    }
    Require-Hash -Value $seal.archive.sha256 -Name 'archive seal SHA-256'
    if ($seal.archive.path -cne "$packageName.zip" -or [UInt64]$seal.archive.size -eq 0 -or
        [UInt64]$seal.archive.size -gt 2GB)
    {
        throw 'Package seal archive identity is invalid or oversized'
    }
}
elseif ($ArchivePath)
{
    throw 'Archive verification requires PackageSealPath'
}

$resolvedArchive = $null
if ($ArchivePath)
{
    $resolvedArchive = [System.IO.Path]::GetFullPath($ArchivePath)
    if (-not (Test-Path -LiteralPath $resolvedArchive -PathType Leaf))
    {
        throw "Package archive does not exist: $resolvedArchive"
    }
    $archiveItem = Get-Item -LiteralPath $resolvedArchive
    if (($archiveItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        [UInt64]$archiveItem.Length -ne [UInt64]$seal.archive.size -or
        (Get-FileSha256 -Path $resolvedArchive) -cne $seal.archive.sha256 -or
        [System.IO.Path]::GetFileName($resolvedArchive) -cne $seal.archive.path)
    {
        throw 'Package archive does not match its external seal'
    }
    $expectedArchiveMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifestFiles)
    {
        $expectedArchiveMap.Add([string]$entry.path, $entry)
    }
    $expectedArchiveMap.Add('package-manifest.json', [ordered]@{
        path = 'package-manifest.json'
        size = [UInt64](Get-Item -LiteralPath $manifestPath).Length
        sha256 = $manifestSha256
    })
    $seenArchivePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedArchive)
    try
    {
        foreach ($entry in $archive.Entries)
        {
            $entryPath = $entry.FullName.Replace('\', '/')
            if ($entryPath.EndsWith('/'))
            {
                throw "Archive has an unexpected directory entry: $entryPath"
            }
            if (-not (Test-RelativePath -Path $entryPath) -or -not $seenArchivePaths.Add($entryPath) -or
                -not $expectedArchiveMap.ContainsKey($entryPath))
            {
                throw "Archive has an invalid, duplicate, or unmanifested entry: $entryPath"
            }
            $expected = $expectedArchiveMap[$entryPath]
            if ([UInt64]$entry.Length -ne [UInt64]$expected.size)
            {
                throw "Archive entry size mismatch: $entryPath"
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
            if ($entrySha256 -cne $expected.sha256)
            {
                throw "Archive entry SHA-256 mismatch: $entryPath"
            }
        }
    }
    finally
    {
        $archive.Dispose()
    }
    if ($seenArchivePaths.Count -ne $expectedArchiveMap.Count)
    {
        throw "Archive entry count mismatch: archive=$($seenArchivePaths.Count) expected=$($expectedArchiveMap.Count)"
    }
}

$result = [ordered]@{
    schema = 'PixelBridge.PortablePackageVerification.1'
    verified = $true
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    packageDirectory = $resolvedPackage
    manifest = [ordered]@{
        path = $manifestPath
        size = [UInt64](Get-Item -LiteralPath $manifestPath).Length
        sha256 = $manifestSha256
    }
    sealPath = $resolvedSeal
    archivePath = $resolvedArchive
    endpointRoles = $endpointRoles
    buildIdentityFingerprintSha256 = $manifest.buildIdentityFingerprintSha256
    testedSourceFingerprintSha256 = $buildIdentity.testedSourceFingerprintSha256
    packagePayloadFingerprintSha256 = $manifest.packagePayloadFingerprintSha256
    packageFileCount = $manifestFiles.Count
    packagePayloadBytes = $manifestPayloadBytes
}
$json = $result | ConvertTo-Json -Depth 10
if ($OutputPath)
{
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
    $temporaryOutput = "$resolvedOutput.partial"
    if ((Test-Path -LiteralPath $resolvedOutput) -or (Test-Path -LiteralPath $temporaryOutput))
    {
        throw "Create-only verification output or partial already exists: $resolvedOutput"
    }
    $parent = [System.IO.Path]::GetDirectoryName($resolvedOutput)
    if ([string]::IsNullOrWhiteSpace($parent) -or -not (Test-Path -LiteralPath $parent -PathType Container))
    {
        throw "Verification output parent does not exist: $parent"
    }
    try
    {
        Write-NewUtf8File -Path $temporaryOutput -Content ($json + "`n")
        Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)
        {
            [System.IO.File]::Delete($temporaryOutput)
        }
    }
}
$json

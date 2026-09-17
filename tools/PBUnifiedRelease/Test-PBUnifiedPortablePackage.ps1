#Requires -Version 7.0
# G22 independent read-only verifier. Never runs a packaged executable.
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
    param([Parameter(Mandatory = $true)][System.IO.Stream]$Stream, [UInt64]$ExpectedBytes)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        $buffer = [byte[]]::new(1MB)
        $total = [UInt64]0
        while ($true) {
            $remainingWithSentinel = $ExpectedBytes - $total + 1
            $count = $Stream.Read($buffer, 0, [int][Math]::Min([UInt64]$buffer.Length, $remainingWithSentinel))
            if ($count -eq 0) { break }
            $total += [UInt64]$count
            if ($total -gt $ExpectedBytes) { throw 'Archive expansion exceeds its sealed entry length' }
            [void]$algorithm.TransformBlock($buffer, 0, $count, $buffer, 0)
        }
        if ($total -ne $ExpectedBytes) { throw 'Archive stream is shorter than its sealed entry length' }
        [void]$algorithm.TransformFinalBlock([byte[]]::new(0), 0, 0)
        return ([BitConverter]::ToString($algorithm.Hash)).Replace('-', '').ToLowerInvariant()
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


function Assert-StrictJsonElement
{
    param([System.Text.Json.JsonElement]$Element)
    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($property in $Element.EnumerateObject()) {
            if (-not $names.Add($property.Name)) { throw "Duplicate or case-ambiguous JSON property: $($property.Name)" }
            Assert-StrictJsonElement -Element $property.Value
        }
    } elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        foreach ($item in $Element.EnumerateArray()) { Assert-StrictJsonElement -Element $item }
    }
}

function Require-UnsignedInteger
{
    param([object]$Value, [string]$Name)
    if (($Value -isnot [Int32] -and $Value -isnot [Int64] -and $Value -isnot [UInt32] -and $Value -isnot [UInt64]) -or $Value -lt 0) {
        throw "$Name must be an unsigned JSON integer, not a string, fraction or boolean"
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
        $text = Get-Content -LiteralPath $Path -Raw -Encoding utf8
        $options = [System.Text.Json.JsonDocumentOptions]::new()
        $options.MaxDepth = 64
        $document = [System.Text.Json.JsonDocument]::Parse($text, $options)
        try { Assert-StrictJsonElement -Element $document.RootElement } finally { $document.Dispose() }
        return $text | ConvertFrom-Json -AsHashtable -Depth 64
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
        if ([string]::IsNullOrWhiteSpace($component) -or $component -eq '.' -or $component -eq '..' -or
            $component -match '[\x00-\x1f<>"|?*]' -or $component.TrimEnd('.', ' ') -cne $component -or
            $component -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)')
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
    $totalBytes = [UInt64]0
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
        $totalBytes += [UInt64]$file.Length
        if ($totalBytes -gt 2GB -or $entries.Count -ge 4096) { throw 'Actual package exceeds total size/count bounds' }
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
if ($manifest.schema -cne 'PixelBridge.UnifiedPortablePackage.1')
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
$isProductRelease = $buildIdentity.releaseScope -ceq 'LocalProductRelease'
if ($buildIdentity.configuration -cne 'Release' -or
    $buildIdentity.releaseScope -cnotin @('LocalCandidateNotPubliclyPublished', 'LocalProductRelease') -or
    $buildIdentity.unifiedProfile.token -cne 'unified' -or $buildIdentity.unifiedProfile.name -cne 'PB-Unified-SC6-V3' -or
    $buildIdentity.unifiedProfile.visualProfileId -ne 5783278666223141683 -or $buildIdentity.unifiedProfile.layoutVersion -ne 10 -or
    $buildIdentity.unifiedProfile.manifestPath -cne 'unified-profile.json' -or
    $buildIdentity.unifiedProfile.manifestSha256 -cne '312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b' -or
    $buildIdentity.unifiedProfile.logicalVisualFpsMinimum -ne 1 -or $buildIdentity.unifiedProfile.logicalVisualFpsDefault -ne 15 -or
    $buildIdentity.unifiedProfile.logicalVisualFpsMaximum -ne 60) {
    throw 'Package has an incompatible Unified SC6-V3 build/profile identity'
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

foreach ($name in @('sourceFileCount', 'packageFileCount', 'packagePayloadBytes')) {
    Require-UnsignedInteger -Value $manifest[$name] -Name $name
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
    Require-UnsignedInteger -Value $entry.size -Name 'source entry size'
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
    Require-UnsignedInteger -Value $entry.size -Name 'package entry size'
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
$allowedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifestFiles) {
    $parts = ([string]$entry.path).Split('/')
    for ($index = 1; $index -lt $parts.Length; $index++) { [void]$allowedDirectories.Add(($parts[0..($index - 1)] -join '/')) }
}
$rootPrefix = $resolvedPackage.TrimEnd('\') + '\'
foreach ($directory in Get-ChildItem -LiteralPath $resolvedPackage -Directory -Recurse -Force) {
    $relative = $directory.FullName.Substring($rootPrefix.Length).Replace('\', '/')
    if (-not $allowedDirectories.Contains($relative) -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected or reparse package directory: $relative"
    }
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
    $identity = $application.runtimeIdentity
    if ($identity.schema -cne 'PixelBridge.ApplicationBuildIdentity.1' -or $identity.applicationName -cne $application.application -or
        $identity.gitCommit -cne $buildIdentity.headCommit -or $identity.protocolMajor -ne 1 -or $identity.protocolMinor -ne 0 -or
        [string]::IsNullOrWhiteSpace([string]$identity.applicationVersion) -or
        $application.versionOutput -cne "$($application.application) $($identity.applicationVersion) (protocol 1.0)") {
        throw 'Recorded application runtime identity/version does not match the sealed build'
    }
    $stream = [IO.File]::OpenRead($executablePath)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw 'Application is not PE/MZ' }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -gt 1MB -or [UInt64]$peOffset + 96 -gt [UInt64]$stream.Length) { throw 'Invalid PE header bounds' }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Application is not PE x64' }
        $stream.Position = $peOffset + 24
        if ($reader.ReadUInt16() -ne 0x20b) { throw 'Application is not PE32+' }
        $stream.Position = $peOffset + 24 + 68
        if ($reader.ReadUInt16() -ne 2) { throw 'Application is not a GUI-subsystem EXE' }
    } finally { $reader.Dispose(); $stream.Dispose() }
    foreach ($vcFile in @($buildIdentity.vcRuntime.files)) {
        $vcPath = if ($endpointRoles.Count -eq 2) { "$($application.role)/$($vcFile.path)" } else { [string]$vcFile.path }
        if (-not $manifestFileMap.ContainsKey($vcPath) -or $manifestFileMap[$vcPath].sha256 -cne $vcFile.sha256 -or
            $manifestFileMap[$vcPath].size -ne $vcFile.size) { throw "VC runtime dependency identity mismatch: $vcPath" }
    }
    $qtCorePath = if ($endpointRoles.Count -eq 2) { "$($application.role)/Qt6Core.dll" } else { 'Qt6Core.dll' }
    if (-not $manifestFileMap.ContainsKey($qtCorePath) -or
        $manifestFileMap[$qtCorePath].sha256 -cne $buildIdentity.qt.deployedQt6CoreSha256)
    {
        throw "Deployed Qt6Core identity is invalid for $($application.role)"
    }
}
foreach ($requiredPackageFile in @('THIRD_PARTY_NOTICES.txt', 'SBOM.spdx.json', 'licenses/qt/LICENSE.txt',
    'licenses/qt/Copyright.txt', 'licenses/qt/licenseInfo.txt', 'licenses/msvc/Redist.txt', 'licenses/msvc/ThirdPartyNotices.txt',
    'unified-profile.json', 'USER_GUIDE.md', 'Test-PBUnifiedPortablePackage.ps1'))
{
    if (-not $manifestFileMap.ContainsKey($requiredPackageFile))
    {
        throw "Required package metadata/license file is absent: $requiredPackageFile"
    }
}
if ($manifestFileMap['unified-profile.json'].sha256 -cne $buildIdentity.unifiedProfile.manifestSha256 -or
    $manifestFileMap['licenses/msvc/Redist.txt'].sha256 -cne $buildIdentity.vcRuntime.redistributableContextSha256 -or
    $manifestFileMap['licenses/msvc/ThirdPartyNotices.txt'].sha256 -cne $buildIdentity.vcRuntime.thirdPartyNoticesSha256 -or
    @($buildIdentity.vcRuntime.files).Count -lt 3 -or @($buildIdentity.vcRuntime.files).Count -gt 32) {
    throw 'Compiled profile or VC runtime/notices binding is invalid'
}
foreach ($requiredVcFile in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (-not (@($buildIdentity.vcRuntime.files.path) -ccontains $requiredVcFile)) { throw "Missing required VC DLL $requiredVcFile" }
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
    $sbom.SPDXID -cne 'SPDXRef-DOCUMENT' -or @($sbom.packages).Count -ne ($vcpkgPackages.Count + 3))
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
$expectedProjectLicense = if ($isProductRelease) { 'MIT' } else { 'NOASSERTION' }
if (-not $sbomPackageMap.ContainsKey('Microsoft Visual C++ Runtime') -or
    $sbomPackageMap['Microsoft Visual C++ Runtime'].versionInfo -cne $buildIdentity.vcRuntime.version -or
    $sbomPackageMap['PixelBridge'].licenseDeclared -cne $expectedProjectLicense) {
    throw 'SPDX SBOM VC runtime/project license does not match the release scope'
}
foreach ($package in $vcpkgPackages)
{
    if (-not $sbomPackageMap.ContainsKey([string]$package.name) -or
        $sbomPackageMap[[string]$package.name].versionInfo -cne $package.version)
    {
        throw "SPDX SBOM is missing the exact vcpkg package identity: $($package.name)"
    }
}

if ($isProductRelease) {
    if ($endpointRoles.Count -ne 1 -or $buildIdentity.qt.version -cne '6.10.1' -or
        $sbomPackageMap['PixelBridge'].licenseConcluded -cne 'MIT' -or
        $sbomPackageMap['Qt'].licenseDeclared -cne 'LGPL-3.0-only') { throw 'Invalid product role or licensing baseline' }
    foreach ($path in @('LICENSE', 'README.md', 'USER_GUIDE.en.md', 'ACKNOWLEDGEMENTS.md', 'THIRD_PARTY_NOTICES.md',
        'RELEASE_V1.0.md', 'QT_SOURCE.md', 'qt.conf', 'sources/qt-source-manifest.json', 'sources/qtbase-6.10.1-source.zip',
        'licenses/qt/LICENSES/LGPL-3.0-only.txt', 'licenses/qt/LICENSES/GPL-3.0-only.txt', 'licenses/qt/qtbase-6.10.1.spdx.json')) {
        if (-not $manifestFileMap.ContainsKey($path)) { throw "Required product release file missing: $path" }
    }
    $sourceLicense = @($sourceFiles | Where-Object { $_.path -ceq 'LICENSE' })
    if ($sourceLicense.Count -ne 1 -or $sourceLicense[0].sha256 -cne $manifestFileMap['LICENSE'].sha256) {
        throw 'Product license is not bound to the committed source inventory'
    }
    $runtimePaths = @("PixelBridge$($endpointRoles[0]).exe", 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
        'blake3.dll', 'zstd.dll', 'platforms/qwindows.dll', 'platforms/qoffscreen.dll', 'styles/qmodernwindowsstyle.dll') +
        @($buildIdentity.vcRuntime.files.path)
    foreach ($path in $runtimePaths) {
        if (-not $manifestFileMap.ContainsKey($path)) { throw "Required product runtime missing: $path" }
    }
    foreach ($path in $manifestFileMap.Keys) {
        if ([IO.Path]::GetExtension($path) -iin @('.dll', '.exe') -and $path -cnotin $runtimePaths) {
            throw "Unapproved product runtime component: $path"
        }
    }
    $qtSource = Read-BoundedJson -Path (Join-Path $resolvedPackage 'sources/qt-source-manifest.json') -MaximumBytes 16MB
    Require-UnsignedInteger -Value $qtSource.fileCount -Name 'Qt source fileCount'
    Require-UnsignedInteger -Value $qtSource.archive.size -Name 'Qt source archive size'
    $qtArchiveEntry = $manifestFileMap['sources/qtbase-6.10.1-source.zip']
    if ($qtSource.schema -cne 'PixelBridge.QtSourceBundle.1' -or $qtSource.version -cne $buildIdentity.qt.version -or
        $qtSource.module -cne 'qtbase' -or $qtSource.archive.path -cne 'qtbase-6.10.1-source.zip' -or
        $qtSource.archive.size -ne $qtArchiveEntry.size -or $qtSource.archive.sha256 -cne $qtArchiveEntry.sha256 -or
        $qtSource.fileCount -lt 1 -or $qtSource.fileCount -gt 65536 -or @($qtSource.files).Count -ne $qtSource.fileCount) {
        throw 'Qt corresponding source binding is invalid'
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
    if ($seal.schema -cne 'PixelBridge.UnifiedPortablePackageSeal.1' -or $seal.packageName -cne $packageName -or
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
            $attributes = [BitConverter]::ToUInt32([BitConverter]::GetBytes([Int32]$entry.ExternalAttributes), 0)
            # Decode the 16-bit Unix mode before comparing: PowerShell's high-bit
            # hex Int32 literal is negative, unlike the normalized UInt32 bits.
            if (($attributes -band 0x400) -ne 0 -or (($attributes -shr 16) -band 0xF000) -eq 0xA000) {
                throw 'Archive entry declares a reparse point or symbolic link'
            }
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
                $entrySha256 = Get-StreamSha256 -Stream $entryStream -ExpectedBytes ([UInt64]$expected.size)
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
    schema = 'PixelBridge.UnifiedPortablePackageVerification.1'
    verified = $true
    packagedExecutablesRun = $false
    authenticityOrPublicLicenseClaim = $false
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

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Encoder', 'Decoder', 'Both')]
    [string]$Role,

    [Parameter(Mandatory = $true)]
    [ValidateSet('current-head', 'instrumented-pre-hardening', 'final-hardening')]
    [string]$Label,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [string[]]$ExcludedSourcePath = @('docs/PHASE1_GATE_REPORT.md'),

    [switch]$CompactPackageName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$normalizedExcludedPaths = @($ExcludedSourcePath | ForEach-Object { $_.Replace('\', '/') })
if ($normalizedExcludedPaths.Count -ne 1 -or
    $normalizedExcludedPaths[0] -cne 'docs/PHASE1_GATE_REPORT.md')
{
    throw 'The only permitted source-fingerprint exclusion is the pre-existing unrelated docs/PHASE1_GATE_REPORT.md'
}

function Write-NewUtf8File
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $encoding = [System.Text.UTF8Encoding]::new($false)
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $writer = [System.IO.StreamWriter]::new($stream, $encoding)
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

function Get-FileSha256
{
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-CanonicalInventoryFingerprint
{
    param([Parameter(Mandatory = $true)][object[]]$Inventory)
    $sortedInventory = @($Inventory | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
    $canonical = ($sortedInventory | ForEach-Object { "$($_.path)`0$($_.size)`0$($_.sha256)`n" }) -join ''
    return Get-TextSha256 -Text $canonical
}

function Get-FileInventory
{
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Files
    )
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $entries = @($Files | ForEach-Object {
        $fullPath = [System.IO.Path]::GetFullPath($_.FullName)
        if (-not $fullPath.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Inventory path escaped root: $fullPath"
        }
        if (($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "Package inventory cannot contain a reparse point: $fullPath"
        }
        [ordered]@{
            path = $fullPath.Substring($resolvedRoot.Length).Replace('\', '/')
            size = [UInt64]$_.Length
            sha256 = Get-FileSha256 -Path $fullPath
        }
    })
    return @($entries | Sort-Object -Property { [string]$_.path } -CaseSensitive -Stable)
}

function Get-CacheValue
{
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $match = Select-String -LiteralPath $CachePath -Pattern ("^" + [regex]::Escape($Name) + ':[^=]+=(.*)$') | Select-Object -First 1
    if ($null -eq $match)
    {
        return $null
    }
    return $match.Matches[0].Groups[1].Value
}

function Get-CmakeSetValue
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $text = [System.IO.File]::ReadAllText($Path)
    $match = [regex]::Match($text, '(?m)^set\(' + [regex]::Escape($Name) + ' "?([^"\)\r\n]*)"?\)')
    if (-not $match.Success)
    {
        throw "CMake compiler identity is missing $Name in $Path"
    }
    return $match.Groups[1].Value
}

function Get-InstalledVcpkgPackages
{
    param([Parameter(Mandatory = $true)][string]$StatusPath)
    $text = [System.IO.File]::ReadAllText($StatusPath)
    $packages = @()
    foreach ($block in [regex]::Split($text.Trim(), '\r?\n\r?\n'))
    {
        $fields = @{}
        foreach ($line in [regex]::Split($block, '\r?\n'))
        {
            $separator = $line.IndexOf(':')
            if ($separator -le 0)
            {
                continue
            }
            $fields[$line.Substring(0, $separator)] = $line.Substring($separator + 1).Trim()
        }
        if ($fields['Status'] -ne 'install ok installed' -or -not $fields.ContainsKey('Package') -or
            -not $fields.ContainsKey('Version') -or -not $fields.ContainsKey('Architecture') -or
            -not $fields.ContainsKey('Abi'))
        {
            continue
        }
        $packages += [ordered]@{
            name = [string]$fields['Package']
            version = [string]$fields['Version']
            portVersion = if ($fields.ContainsKey('Port-Version')) { [UInt32]$fields['Port-Version'] } else { 0 }
            architecture = [string]$fields['Architecture']
            abi = [string]$fields['Abi']
        }
    }
    return @($packages | Sort-Object -Property { [string]$_.name } -CaseSensitive -Stable)
}

function Copy-ApplicationDirectory
{
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $reparsePoint = Get-ChildItem -LiteralPath $Source -Force -Recurse | Where-Object {
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
    } | Select-Object -First 1
    if ($null -ne $reparsePoint)
    {
        throw "Application directory contains a reparse point: $($reparsePoint.FullName)"
    }
    if (-not (Test-Path -LiteralPath $Destination -PathType Container))
    {
        [void](New-Item -ItemType Directory -Path $Destination)
    }
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$resolvedBuild = [System.IO.Path]::GetFullPath($BuildDirectory)
$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot '.git') -PathType Container))
{
    throw "Repository root is not a Git worktree: $repositoryRoot"
}
if (-not (Test-Path -LiteralPath $resolvedBuild -PathType Container))
{
    throw "Build directory does not exist: $resolvedBuild"
}
if (-not (Test-Path -LiteralPath $resolvedOutputRoot -PathType Container))
{
    [void](New-Item -ItemType Directory -Path $resolvedOutputRoot)
}

$cachePath = Join-Path $resolvedBuild 'CMakeCache.txt'
$vcpkgStatusPath = Join-Path $resolvedBuild 'vcpkg_installed\vcpkg\status'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $vcpkgStatusPath -PathType Leaf))
{
    throw 'Build directory is missing CMakeCache.txt or the vcpkg installed status database'
}

$headCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
$headTree = (& git -C $repositoryRoot rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0 -or $headCommit -notmatch '^[0-9a-f]{40}$' -or $headTree -notmatch '^[0-9a-f]{40}$')
{
    throw 'Unable to resolve HEAD commit/tree identity'
}
$tagIdentityScript = Join-Path $repositoryRoot 'cmake\PBVerifyPhase1GateTagIdentity.cmake'
if (-not (Test-Path -LiteralPath $tagIdentityScript -PathType Leaf))
{
    throw "Step 01 tag identity check is missing: $tagIdentityScript"
}
$null = & cmake "-DPB_REPOSITORY_ROOT=$repositoryRoot" -P $tagIdentityScript
if ($LASTEXITCODE -ne 0)
{
    throw "Phase-1 Gate tag identity check failed (exit $LASTEXITCODE)"
}
$tagObject = (& git -C $repositoryRoot rev-parse phase1-gate-pass).Trim()
$tagCommit = (& git -C $repositoryRoot rev-parse 'phase1-gate-pass^{}').Trim()
if ($LASTEXITCODE -ne 0 -or $tagObject -notmatch '^[0-9a-f]{40}$' -or $tagCommit -notmatch '^[0-9a-f]{40}$')
{
    throw 'Unable to resolve phase1-gate-pass identity'
}

$sourcePaths = @(& git -C $repositoryRoot -c core.quotepath=false ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0)
{
    throw 'Unable to enumerate source tree'
}
$excluded = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($path in $ExcludedSourcePath)
{
    [void]$excluded.Add($path.Replace('\', '/'))
}
$sourceInventory = @($sourcePaths | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_) -and -not $excluded.Contains($_.Replace('\', '/'))
} | Sort-Object -CaseSensitive | ForEach-Object {
    $relativePath = $_.Replace('\', '/')
    if ($relativePath.Contains("`r") -or $relativePath.Contains("`n"))
    {
        throw "Unsupported source path contains a line break: $relativePath"
    }
    $absolutePath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $_))
    $repositoryPrefix = $repositoryRoot.TrimEnd('\') + '\'
    if (-not $absolutePath.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $absolutePath -PathType Leaf))
    {
        throw "Source inventory path is invalid: $relativePath"
    }
    $item = Get-Item -LiteralPath $absolutePath
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Source inventory cannot contain a reparse point: $relativePath"
    }
    [ordered]@{
        path = $relativePath
        size = [UInt64]$item.Length
        sha256 = Get-FileSha256 -Path $absolutePath
    }
})
if ($sourceInventory.Count -eq 0 -or $sourceInventory.Count -gt 65536)
{
    throw "Source inventory is outside 1..65536: $($sourceInventory.Count)"
}
$sourceFingerprint = Get-CanonicalInventoryFingerprint -Inventory $sourceInventory

$compilerFiles = @(Get-ChildItem -LiteralPath (Join-Path $resolvedBuild 'CMakeFiles') -Recurse -Filter 'CMakeCXXCompiler.cmake' -File)
if ($compilerFiles.Count -ne 1)
{
    throw "Expected exactly one CMakeCXXCompiler.cmake, found $($compilerFiles.Count)"
}
$compilerPath = [System.IO.Path]::GetFullPath((Get-CmakeSetValue -Path $compilerFiles[0].FullName -Name 'CMAKE_CXX_COMPILER'))
if (-not (Test-Path -LiteralPath $compilerPath -PathType Leaf))
{
    throw "Configured C++ compiler does not exist: $compilerPath"
}
$compiler = [ordered]@{
    id = Get-CmakeSetValue -Path $compilerFiles[0].FullName -Name 'CMAKE_CXX_COMPILER_ID'
    version = Get-CmakeSetValue -Path $compilerFiles[0].FullName -Name 'CMAKE_CXX_COMPILER_VERSION'
    architecture = Get-CmakeSetValue -Path $compilerFiles[0].FullName -Name 'CMAKE_CXX_COMPILER_ARCHITECTURE_ID'
    path = $compilerPath
    size = [UInt64](Get-Item -LiteralPath $compilerPath).Length
    sha256 = Get-FileSha256 -Path $compilerPath
}

$windowsSdkVersions = @(Get-ChildItem -LiteralPath (Join-Path $resolvedBuild 'CMakeFiles') -Recurse -Filter '*.vcxproj' -File | ForEach-Object {
    $match = [regex]::Match([System.IO.File]::ReadAllText($_.FullName), '<WindowsTargetPlatformVersion>([^<]+)</WindowsTargetPlatformVersion>')
    if ($match.Success) { $match.Groups[1].Value }
} | Sort-Object -Unique)
if ($windowsSdkVersions.Count -ne 1)
{
    throw "Expected one Windows SDK target version, found: $($windowsSdkVersions -join ', ')"
}

$qt6Dir = Get-CacheValue -CachePath $cachePath -Name 'Qt6_DIR'
if ([string]::IsNullOrWhiteSpace($qt6Dir))
{
    throw 'CMake cache has no Qt6_DIR'
}
$qtRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent ([System.IO.Path]::GetFullPath($qt6Dir))))
$qtInstallationRoot = Split-Path -Parent (Split-Path -Parent $qtRoot)
$qtLicenseRoot = Join-Path $qtInstallationRoot 'Licenses'
$qtLicenseInfoPath = Join-Path $qtInstallationRoot 'licenseInfo.txt'
foreach ($requiredQtLicense in @((Join-Path $qtLicenseRoot 'LICENSE'), (Join-Path $qtLicenseRoot 'Copyright.txt'), $qtLicenseInfoPath))
{
    if (-not (Test-Path -LiteralPath $requiredQtLicense -PathType Leaf))
    {
        throw "Qt license input is missing: $requiredQtLicense"
    }
}

$vcpkgManifestPath = Join-Path $repositoryRoot 'vcpkg.json'
$vcpkgConfigurationPath = Join-Path $repositoryRoot 'vcpkg-configuration.json'
$vcpkgManifest = Get-Content -LiteralPath $vcpkgManifestPath -Raw | ConvertFrom-Json
if ($vcpkgManifest.'builtin-baseline' -notmatch '^[0-9a-f]{40}$')
{
    throw 'vcpkg builtin-baseline is missing or invalid'
}
$vcpkgPackages = @(Get-InstalledVcpkgPackages -StatusPath $vcpkgStatusPath)
$requiredRuntimePackages = @('blake3', 'libpng', 'wirehair', 'zlib', 'zstd')
foreach ($requiredPackage in $requiredRuntimePackages)
{
    if (-not ($vcpkgPackages.name -ccontains $requiredPackage))
    {
        throw "Required runtime vcpkg package is absent: $requiredPackage"
    }
}
foreach ($package in $vcpkgPackages)
{
    $copyrightPath = Join-Path $resolvedBuild "vcpkg_installed\x64-windows\share\$($package.name)\copyright"
    if (-not (Test-Path -LiteralPath $copyrightPath -PathType Leaf))
    {
        throw "Installed vcpkg package copyright is absent: $copyrightPath"
    }
}

$applicationRoles = if ($Role -eq 'Both') { @('Encoder', 'Decoder') } else { @($Role) }
$applicationSources = @{}
foreach ($applicationRole in $applicationRoles)
{
    $applicationName = "PixelBridge$applicationRole"
    $applicationDirectory = Join-Path $resolvedBuild "apps\$applicationName\Release"
    $applicationExecutable = Join-Path $applicationDirectory "$applicationName.exe"
    if (-not (Test-Path -LiteralPath $applicationExecutable -PathType Leaf))
    {
        throw "Release executable does not exist: $applicationExecutable"
    }
    $applicationSources[$applicationRole] = $applicationDirectory
}

$qtCoreCandidates = @($applicationRoles | ForEach-Object {
    Join-Path $applicationSources[$_] 'Qt6Core.dll'
})
foreach ($qtCorePath in $qtCoreCandidates)
{
    if (-not (Test-Path -LiteralPath $qtCorePath -PathType Leaf))
    {
        throw "Deployed Qt6Core.dll is missing: $qtCorePath"
    }
}
$qtCoreHashes = @($qtCoreCandidates | ForEach-Object { Get-FileSha256 -Path $_ } | Sort-Object -Unique)
$qtVersions = @($qtCoreCandidates | ForEach-Object {
    [Diagnostics.FileVersionInfo]::GetVersionInfo([System.IO.Path]::GetFullPath($_)).ProductVersion
} | Sort-Object -Unique)
if ($qtCoreHashes.Count -ne 1 -or $qtVersions.Count -ne 1 -or [string]::IsNullOrWhiteSpace($qtVersions[0]))
{
    throw 'Encoder/Decoder deployed Qt6Core identities are inconsistent'
}

$cmakeGenerator = Get-CacheValue -CachePath $cachePath -Name 'CMAKE_GENERATOR'
$cmakeGeneratorPlatform = Get-CacheValue -CachePath $cachePath -Name 'CMAKE_GENERATOR_PLATFORM'
$vcpkgTriplet = Get-CacheValue -CachePath $cachePath -Name 'VCPKG_TARGET_TRIPLET'
if ([string]::IsNullOrWhiteSpace($cmakeGenerator) -or [string]::IsNullOrWhiteSpace($vcpkgTriplet))
{
    throw 'CMake generator or vcpkg target triplet is missing'
}
$createdUtc = [DateTime]::UtcNow.ToString('o')
$buildIdentity = [ordered]@{
    headCommit = $headCommit
    headTree = $headTree
    testedSourceFingerprintSha256 = $sourceFingerprint
    phase1GatePassTagObject = $tagObject
    phase1GatePassCommit = $tagCommit
    configuration = 'Release'
    cmake = [ordered]@{
        version = (& cmake --version | Select-Object -First 1)
        generator = $cmakeGenerator
        generatorPlatform = $cmakeGeneratorPlatform
        cacheSha256 = Get-FileSha256 -Path $cachePath
    }
    compiler = $compiler
    windowsSdkVersion = $windowsSdkVersions[0]
    qt = [ordered]@{
        version = $qtVersions[0]
        deployedQt6CoreSha256 = $qtCoreHashes[0]
        configuredRoot = $qtRoot
        licenseInfoSha256 = Get-FileSha256 -Path $qtLicenseInfoPath
    }
    vcpkg = [ordered]@{
        builtinBaseline = [string]$vcpkgManifest.'builtin-baseline'
        targetTriplet = $vcpkgTriplet
        manifestSha256 = Get-FileSha256 -Path $vcpkgManifestPath
        configurationSha256 = if (Test-Path -LiteralPath $vcpkgConfigurationPath -PathType Leaf) {
            Get-FileSha256 -Path $vcpkgConfigurationPath
        } else { $null }
        installedStatusSha256 = Get-FileSha256 -Path $vcpkgStatusPath
        packages = $vcpkgPackages
    }
    remoteVisualLowFpsProfile = [ordered]@{
        token = 'remote-lf4'
        visualProfileId = [UInt64]5783275402097472561
        layoutVersion = 7
        codedDataBytesPerFrame = 8100
        codewordsPerFrame = 4
        logicalVisualFpsMinimum = 1
        logicalVisualFpsMaximum = 5
    }
}
$buildIdentityFingerprint = Get-TextSha256 -Text ($buildIdentity | ConvertTo-Json -Depth 16 -Compress)
$packageName = if ($CompactPackageName)
{
    $compactRole = switch ($Role)
    {
        'Encoder' { 'E' }
        'Decoder' { 'D' }
        'Both' { 'B' }
    }
    "PB-RV-$compactRole-$($headCommit.Substring(0, 8))-$($sourceFingerprint.Substring(0, 8))"
}
else
{
    "PixelBridge-RemoteVisual-$Label-$Role-$($headCommit.Substring(0, 8))-$($sourceFingerprint.Substring(0, 12))"
}
$packageDirectory = Join-Path $resolvedOutputRoot $packageName
$stagingDirectory = Join-Path $resolvedOutputRoot "$packageName.partial"
$zipPath = Join-Path $resolvedOutputRoot "$packageName.zip"
$temporaryZipPath = Join-Path $resolvedOutputRoot "$packageName.partial.zip"
$sealPath = Join-Path $resolvedOutputRoot "$packageName.seal.json"
$temporarySealPath = Join-Path $resolvedOutputRoot "$packageName.partial.seal.json"
foreach ($path in @($packageDirectory, $stagingDirectory, $zipPath, $temporaryZipPath, $sealPath, $temporarySealPath))
{
    if (Test-Path -LiteralPath $path)
    {
        throw "Create-only output already exists: $path"
    }
}

[void](New-Item -ItemType Directory -Path $stagingDirectory)
$published = $false
try
{
    $applications = @()
    foreach ($applicationRole in $applicationRoles)
    {
        $applicationName = "PixelBridge$applicationRole"
        $destination = if ($Role -eq 'Both') { Join-Path $stagingDirectory $applicationRole } else { $stagingDirectory }
        Copy-ApplicationDirectory -Source $applicationSources[$applicationRole] -Destination $destination
        $relativeExecutablePath = if ($Role -eq 'Both') {
            "$applicationRole/$applicationName.exe"
        } else {
            "$applicationName.exe"
        }
        $copiedExecutable = Join-Path $stagingDirectory ($relativeExecutablePath.Replace('/', '\'))
        $applications += [ordered]@{
            role = $applicationRole
            application = $applicationName
            relativeExecutablePath = $relativeExecutablePath
            size = [UInt64](Get-Item -LiteralPath $copiedExecutable).Length
            sha256 = Get-FileSha256 -Path $copiedExecutable
            versionOutput = (& $copiedExecutable --version)
        }
        if ($LASTEXITCODE -ne 0)
        {
            throw "$applicationName --version failed with exit code $LASTEXITCODE"
        }
    }

    $licenseDirectory = Join-Path $stagingDirectory 'licenses'
    $qtPackageLicenseDirectory = Join-Path $licenseDirectory 'qt'
    $vcpkgPackageLicenseDirectory = Join-Path $licenseDirectory 'vcpkg'
    [void](New-Item -ItemType Directory -Path $qtPackageLicenseDirectory -Force)
    [void](New-Item -ItemType Directory -Path $vcpkgPackageLicenseDirectory -Force)
    Copy-Item -LiteralPath (Join-Path $qtLicenseRoot 'LICENSE') -Destination (Join-Path $qtPackageLicenseDirectory 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $qtLicenseRoot 'Copyright.txt') -Destination (Join-Path $qtPackageLicenseDirectory 'Copyright.txt')
    Copy-Item -LiteralPath $qtLicenseInfoPath -Destination (Join-Path $qtPackageLicenseDirectory 'licenseInfo.txt')
    foreach ($package in $vcpkgPackages)
    {
        $copyrightPath = Join-Path $resolvedBuild "vcpkg_installed\x64-windows\share\$($package.name)\copyright"
        Copy-Item -LiteralPath $copyrightPath -Destination (Join-Path $vcpkgPackageLicenseDirectory "$($package.name).txt")
    }

    $noticeLines = [Collections.Generic.List[string]]::new()
    $noticeLines.Add('PixelBridge THIRD_PARTY_NOTICES')
    $noticeLines.Add("Generated for package $packageName")
    $noticeLines.Add("Qt $($qtVersions[0]): licenses/qt/LICENSE.txt, Copyright.txt, licenseInfo.txt")
    foreach ($package in $vcpkgPackages)
    {
        $noticePath = "licenses/vcpkg/$($package.name).txt"
        $noticeLines.Add("vcpkg $($package.name) $($package.version) port $($package.portVersion) ABI $($package.abi): $noticePath")
    }
    Write-NewUtf8File -Path (Join-Path $stagingDirectory 'THIRD_PARTY_NOTICES.txt') -Content (($noticeLines -join "`n") + "`n")

    $spdxPackages = @([ordered]@{
        name = 'PixelBridge'
        SPDXID = 'SPDXRef-Package-PixelBridge'
        versionInfo = '0.1.0'
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        copyrightText = 'NOASSERTION'
    })
    $spdxPackages += [ordered]@{
        name = 'Qt'
        SPDXID = 'SPDXRef-Package-Qt'
        versionInfo = $qtVersions[0]
        downloadLocation = 'https://www.qt.io/'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        copyrightText = 'See licenses/qt/ in this package'
    }
    foreach ($package in $vcpkgPackages)
    {
        $safeName = [regex]::Replace($package.name, '[^A-Za-z0-9.-]', '-')
        $spdxPackages += [ordered]@{
            name = $package.name
            SPDXID = "SPDXRef-Package-vcpkg-$safeName"
            versionInfo = $package.version
            downloadLocation = 'NOASSERTION'
            filesAnalyzed = $false
            licenseConcluded = 'NOASSERTION'
            licenseDeclared = 'NOASSERTION'
            copyrightText = "See licenses/vcpkg/$($package.name).txt when present"
            externalRefs = @([ordered]@{
                referenceCategory = 'PACKAGE-MANAGER'
                referenceType = 'purl'
                referenceLocator = "pkg:vcpkg/$($package.name)@$($package.version)?triplet=$($package.architecture)&abi=$($package.abi)"
            })
        }
    }
    $relationships = @($spdxPackages | Select-Object -Skip 1 | ForEach-Object {
        [ordered]@{
            spdxElementId = 'SPDXRef-Package-PixelBridge'
            relationshipType = 'DEPENDS_ON'
            relatedSpdxElement = $_.SPDXID
        }
    })
    $sbom = [ordered]@{
        spdxVersion = 'SPDX-2.3'
        dataLicense = 'CC0-1.0'
        SPDXID = 'SPDXRef-DOCUMENT'
        name = "$packageName SBOM"
        documentNamespace = "https://pixelbridge.local/spdx/$packageName/$buildIdentityFingerprint"
        creationInfo = [ordered]@{
            created = $createdUtc
            creators = @('Tool: New-PBRemoteVisualPortablePackage.ps1')
        }
        documentDescribes = @('SPDXRef-Package-PixelBridge')
        packages = $spdxPackages
        relationships = $relationships
    }
    Write-NewUtf8File -Path (Join-Path $stagingDirectory 'SBOM.spdx.json') -Content ($sbom | ConvertTo-Json -Depth 16)

    $packageFiles = @(Get-ChildItem -LiteralPath $stagingDirectory -File -Recurse -Force)
    if ($packageFiles.Count -eq 0 -or $packageFiles.Count -gt 4096)
    {
        throw "Package file inventory is outside 1..4096: $($packageFiles.Count)"
    }
    $packageBytes = [UInt64](($packageFiles | Measure-Object -Property Length -Sum).Sum)
    if ($packageBytes -gt 2GB)
    {
        throw "Package exceeds the 2 GiB evidence bound: $packageBytes"
    }
    $packageInventory = @(Get-FileInventory -Root $stagingDirectory -Files $packageFiles)
    $packagePayloadFingerprint = Get-CanonicalInventoryFingerprint -Inventory $packageInventory
    $manifest = [ordered]@{
        schema = 'PixelBridge.PortablePackage.2'
        createdUtc = $createdUtc
        packageName = $packageName
        label = $Label
        endpointRoles = $applicationRoles
        applications = $applications
        buildIdentity = $buildIdentity
        buildIdentityFingerprintSha256 = $buildIdentityFingerprint
        excludedUnrelatedSourcePaths = @($ExcludedSourcePath)
        sourceFileCount = $sourceInventory.Count
        sourceFiles = $sourceInventory
        buildDirectory = $resolvedBuild
        packageFileCount = $packageFiles.Count
        packagePayloadBytes = $packageBytes
        packagePayloadFingerprintSha256 = $packagePayloadFingerprint
        files = $packageInventory
    }
    $manifestPath = Join-Path $stagingDirectory 'package-manifest.json'
    Write-NewUtf8File -Path $manifestPath -Content ($manifest | ConvertTo-Json -Depth 24)

    Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $stagingDirectory -Force).FullName `
        -DestinationPath $temporaryZipPath -CompressionLevel Optimal
    $seal = [ordered]@{
        schema = 'PixelBridge.PortablePackageSeal.2'
        packageName = $packageName
        label = $Label
        endpointRoles = $applicationRoles
        headCommit = $headCommit
        headTree = $headTree
        testedSourceFingerprintSha256 = $sourceFingerprint
        buildIdentityFingerprintSha256 = $buildIdentityFingerprint
        packagePayloadFingerprintSha256 = $packagePayloadFingerprint
        archive = [ordered]@{
            path = [System.IO.Path]::GetFileName($zipPath)
            size = [UInt64](Get-Item -LiteralPath $temporaryZipPath).Length
            sha256 = Get-FileSha256 -Path $temporaryZipPath
        }
        manifest = [ordered]@{
            path = 'package-manifest.json'
            size = [UInt64](Get-Item -LiteralPath $manifestPath).Length
            sha256 = Get-FileSha256 -Path $manifestPath
        }
    }
    Write-NewUtf8File -Path $temporarySealPath -Content ($seal | ConvertTo-Json -Depth 12)

    Move-Item -LiteralPath $stagingDirectory -Destination $packageDirectory
    Move-Item -LiteralPath $temporaryZipPath -Destination $zipPath
    Move-Item -LiteralPath $temporarySealPath -Destination $sealPath
    $verificationScript = Join-Path $PSScriptRoot 'Test-PBRemoteVisualPortablePackage.ps1'
    if (-not (Test-Path -LiteralPath $verificationScript -PathType Leaf))
    {
        throw "Portable-package verifier is missing: $verificationScript"
    }
    $verification = & $verificationScript -PackageDirectory $packageDirectory -PackageSealPath $sealPath -ArchivePath $zipPath
    $published = $true
    [ordered]@{
        packageDirectory = $packageDirectory
        archivePath = $zipPath
        sealPath = $sealPath
        manifestSha256 = $seal.manifest.sha256
        archiveSha256 = $seal.archive.sha256
        buildIdentityFingerprintSha256 = $buildIdentityFingerprint
        testedSourceFingerprintSha256 = $sourceFingerprint
        packagePayloadFingerprintSha256 = $packagePayloadFingerprint
        verification = $verification
    } | ConvertTo-Json -Depth 16
}
finally
{
    if (-not $published)
    {
        foreach ($path in @($stagingDirectory, $temporaryZipPath, $temporarySealPath, $packageDirectory, $zipPath, $sealPath))
        {
            if (Test-Path -LiteralPath $path)
            {
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
}

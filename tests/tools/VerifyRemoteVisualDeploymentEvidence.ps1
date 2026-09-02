[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolsRoot,

    [Parameter(Mandatory = $true)]
    [string]$WorkRoot
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
if (-not (Test-Path -LiteralPath $resolvedToolsRoot -PathType Container))
{
    throw "RemoteVisual evidence tool root does not exist: $resolvedToolsRoot"
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

#Requires -Version 5.1
# Manual field entry. Default is verification/command preview only.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'Decoder')][string]$Role,
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$SealPath,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedManifestSha256,
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [switch]$AllowGuiLaunch
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath($PackageDirectory)
$evidence = [System.IO.Path]::GetFullPath($EvidenceRoot)
if ($EvidenceRoot -notmatch '^[A-Za-z]:[\\/]' -or $EvidenceRoot -match '["\x00-\x1f]' -or
    (Test-Path -LiteralPath $evidence) -or !(Test-Path -LiteralPath ([System.IO.Path]::GetDirectoryName($evidence)) -PathType Container))
{
    throw 'EvidenceRoot must be a new absolute local directory with an existing parent'
}
& (Join-Path $PSScriptRoot 'Test-PBStep1Package.ps1') -PackageDirectory $root -SealPath $SealPath -ExpectedManifestSha256 $ExpectedManifestSha256
$executable = Join-Path $root ($Role + '/PixelBridge' + $Role + '.exe')
$arguments = '--gui-measurement --evidence-root "' + $evidence + '"'
if (!$AllowGuiLaunch)
{
    [ordered]@{ started = $false; executable = $executable; arguments = $arguments;
        prerequisite = 'Explicit field approval; manually verify protected/right-screen topology; manual GUI Start only';
        noInputAutomation = $true } | ConvertTo-Json
    return
}
# A visible interactive GUI is explicitly requested by -AllowGuiLaunch. This
# never clicks Start/Stop, manipulates the cursor, or launches a capture CLI.
Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Normal | Out-Null

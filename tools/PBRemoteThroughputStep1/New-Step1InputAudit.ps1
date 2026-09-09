#Requires -Version 5.1
# Portable sender pre/post audit. No Python install, window, input or network.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$SealPath,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedManifestSha256,
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Test-PBStep1Package.ps1') -PackageDirectory $PackageDirectory -SealPath $SealPath -ExpectedManifestSha256 $ExpectedManifestSha256 | Out-Null
$source = [System.IO.Path]::GetFullPath($SourcePath)
$output = [System.IO.Path]::GetFullPath($OutputPath)
$ledgerPath = $output + '.ledger.json'
if ($SourcePath -notmatch '^[A-Za-z]:[\\/]' -or $OutputPath -notmatch '^[A-Za-z]:[\\/]' -or
    $source -match '["\x00-\x1f]' -or $output -match '["\x00-\x1f]' -or
    (Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $ledgerPath)) { throw 'Use explicit local source and new output paths' }
foreach ($path in @($source, [System.IO.Path]::GetDirectoryName($output)))
{
    $current = $path
    while ($current)
    {
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse path rejected' }
        $current = [System.IO.Path]::GetDirectoryName($current)
    }
}
if (-not ('PBStep1Audit.FileIdentity' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace PBStep1Audit
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Facts
    {
        public uint attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME creation, access, write;
        public uint volume, sizeHigh, sizeLow, links, indexHigh, indexLow;
    }
    public static class FileIdentity
    {
        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetFileInformationByHandle(IntPtr handle, out Facts facts);
    }
}
'@
}
$lease = [System.IO.File]::Open($source, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
try
{
    if ($lease.Length -gt 64MB) { throw 'Step1 source limit is 64 MiB' }
    $before = Get-Item -LiteralPath $source
    $facts = [PBStep1Audit.Facts]::new()
    if (![PBStep1Audit.FileIdentity]::GetFileInformationByHandle($lease.SafeFileHandle.DangerousGetHandle(), [ref]$facts)) { throw 'Source file identity unavailable' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $sourceSha = [BitConverter]::ToString($sha.ComputeHash($lease)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PackageDirectory 'Encoder/PBStep1SourceAudit.exe'
    $start.Arguments = '--source "' + $source + '" --output "' + $ledgerPath + '"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    try
    {
        if (!$process.Start()) { throw 'Source audit did not start' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(60000))
        {
            $process.Kill()
            throw 'Source audit exceeded 60 seconds; partial evidence retained'
        }
        if ($process.ExitCode -ne 0) { throw ('Source audit failed: ' + $stderr.Result) }
        if ($stdout.Result.Length -gt 65536) { throw 'Unexpected oversized audit response' }
    }
    finally { $process.Dispose() }
    $after = Get-Item -LiteralPath $source
    if ($after.Length -ne $before.Length -or $after.LastWriteTimeUtc -ne $before.LastWriteTimeUtc) { throw 'Source changed' }
    if ((Get-Item -LiteralPath $ledgerPath).Length -gt 65536) { throw 'Ledger record limit' }
    $ledger = Get-Content -LiteralPath $ledgerPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($ledger.schema -cne 'PixelBridge.Step1.SourceLedger.1' -or $ledger.complete -ne $true -or $ledger.rawBytes -ne $lease.Length) { throw 'Ledger/source mismatch' }
    $segments = @($ledger.segments | ForEach-Object { [ordered]@{ rawBytes = $_.rawBytes; rawBlake3 = $_.rawBlake3 } })
    $unixNanoseconds = [long](($before.LastWriteTimeUtc.Ticks - 621355968000000000L) * 100L)
    $audit = [ordered]@{
        schema = 'PixelBridge.Step1.InputAudit.1'; source = $source
        identity = [ordered]@{ bytes = $lease.Length; sha256 = $sourceSha; blake3 = $ledger.blake3; segments = $segments;
            fileId = $facts.indexHigh.ToString('x8') + $facts.indexLow.ToString('x8'); device = $facts.volume.ToString(); mtimeNs = $unixNanoseconds.ToString() }
        ledger = $ledger
        ledgerFile = [ordered]@{ size = (Get-Item -LiteralPath $ledgerPath).Length;
            sha256 = (Get-FileHash -LiteralPath $ledgerPath -Algorithm SHA256).Hash.ToLowerInvariant() }
        independentDigestImplementation = 'PowerShell/.NET SHA256 + portable PBStep1SourceAudit BLAKE3; immutable read lease across both'
    }
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes(($audit | ConvertTo-Json -Depth 12))
    $file = [System.IO.File]::Open($output, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) }
    finally { $file.Dispose() }
    [ordered]@{ auditPath = $output; bytes = $lease.Length; sha256 = $sourceSha; blake3 = $ledger.blake3; encodedBytes = $ledger.encodedBytes } | ConvertTo-Json
}
finally { $lease.Dispose() }

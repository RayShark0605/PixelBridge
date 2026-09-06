param(
    [ValidateSet('smoke', 'full')][string]$Stage = 'smoke',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$executable = Join-Path $root 'bin\PixelBridgeEncoder.exe'
$expected = Get-Content -LiteralPath (Join-Path $root 'expected-build.json') -Raw | ConvertFrom-Json
$actualHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expected.encoderSha256) { throw 'Encoder executable hash mismatch. Re-extract the original kit.' }
$identityText = & $executable --build-identity
if ($LASTEXITCODE -ne 0) { throw 'Encoder cannot load. Keep the entire bin folder including all DLLs and subfolders.' }
$identity = ($identityText -join "`n") | ConvertFrom-Json
if ($identity.gitCommit -ne $expected.gitCommit -or $identity.applicationName -ne 'PixelBridgeEncoder') { throw 'Unexpected Encoder build identity.' }
if ($CheckOnly)
{
    Write-Host ('PASS: Encoder loads; commit ' + $identity.gitCommit)
    Write-Host 'No source generated; no window or transmission started.'
    exit 0
}
function WriteNewText([string]$Path, [string]$Text)
{
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
}
$runId = [Guid]::NewGuid().ToString('N')
$runRoot = Join-Path $root ('runs\' + $Stage + '-' + $runId)
if (Test-Path -LiteralPath $runRoot) { throw 'Create-only run directory already exists.' }
New-Item -ItemType Directory -Path $runRoot | Out-Null
$size = if ($Stage -eq 'smoke') { 1MB } else { 64MB }
$source = Join-Path $runRoot ('g21-' + $Stage + '-' + $runId + '.bin')
$random = [Security.Cryptography.RandomNumberGenerator]::Create()
$file = [IO.File]::Open($source, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try
{
    $buffer = New-Object byte[] (1MB)
    for ($written = 0; $written -lt $size; $written += $buffer.Length)
    {
        $random.GetBytes($buffer)
        $file.Write($buffer, 0, $buffer.Length)
    }
    $file.Flush($true)
}
finally { $file.Dispose(); $random.Dispose() }
$sourceLease = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$maximumSeconds = if ($Stage -eq 'smoke') { 600 } else { 1800 }
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$reportPath = Join-Path $runRoot 'encoder-report.json'
$journalPath = Join-Path $runRoot 'encoder-evidence.jsonl'
$command = @('--headless-broadcast', '--source', $source, '--profile', 'unified', '--channel', 'remote', '--remote-provider', 'UnknownRemoteLink',
    '--single-monitor-fullscreen', 'primary', '--logical-fps', '15', '--seconds', $maximumSeconds.ToString([Globalization.CultureInfo]::InvariantCulture), '--manual-stop', '--run-id', $runId,
    '--report', $reportPath, '--journal', $journalPath)
$manifest = [ordered]@{
    schema = 'PixelBridge.G21.SenderFixture.1'; stage = $Stage; runId = $runId
    sourceName = [IO.Path]::GetFileName($source); bytes = $size; sha256 = $sourceHash
    generator = 'Remote-host OS CSPRNG; bounded 1 MiB buffer; fresh source per run'
    sourceTransferredToDecoder = $false; configuredLogicalFps = 15; hardMaximumBroadcastSeconds = $maximumSeconds
    presentationMode = 'Primary monitor borderless fullscreen; exact 1920x1080 canvas centered at 1:1 in neutral matte'
    build = $identity; encoderSha256 = $actualHash; arguments = $command
}
WriteNewText (Join-Path $runRoot 'source-manifest.json') ($manifest | ConvertTo-Json -Depth 5)
Write-Host ('RUN: ' + $runRoot)
Write-Host ('SOURCE SHA256: ' + $sourceHash)
Write-Host 'Keep the entire animated canvas visible in the remote view on the LOCAL RIGHT screen.'
Write-Host 'Do not minimize/cover the canvas. Do not send the .bin source back to the receiver.'
Write-Host 'When Codex asks you to stop: focus THIS console on the remote PC and press Enter or Q.'
Write-Host ('The run has a ' + $maximumSeconds + '-second safety limit. A limit exit is not receiver success.')
try
{
    & $executable @command
    $encoderExit = $LASTEXITCODE
}
finally { $sourceLease.Dispose() }
WriteNewText (Join-Path $runRoot 'process-exit.json') (@{ encoderExit = $encoderExit; finishedUtc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json)
& (Join-Path $root 'Get-G21FileDigests.ps1') -InputPath $source -OutputPath (Join-Path $runRoot 'source-poststop-digests.json') -Blake3Dll (Join-Path $root 'bin\blake3.dll') -ExpectedDllSha256 $expected.blake3Sha256
Write-Host ('Encoder stopped with exit ' + $encoderExit + '. Keep ALL run evidence.')
Write-Host 'After receiver termination, share source-manifest.json, source-poststop-digests.json, encoder-report.json and process-exit.json; never the source .bin.'
exit $encoderExit

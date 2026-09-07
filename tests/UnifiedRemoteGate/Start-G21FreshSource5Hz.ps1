param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function ReadSmallJson([string]$Path)
{
    $item = Get-Item -LiteralPath $Path
    if ($item.PSIsContainer -or $item.Length -gt 16384 -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint))
    {
        throw 'Expected metadata must be a regular file no larger than 16 KiB.'
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function WriteNewText([string]$Path, [string]$Text)
{
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
}

$executable = Join-Path $root 'bin\PixelBridgeEncoder.exe'
$expected = ReadSmallJson (Join-Path $root 'expected-build.json')
if ($expected.gitCommit -cnotmatch '^[0-9a-f]{40}$' -or $expected.encoderSha256 -cnotmatch '^[0-9a-f]{64}$')
{
    throw 'Expected build metadata is malformed.'
}
$actualHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -cne $expected.encoderSha256) { throw 'Encoder executable hash mismatch. Re-extract this complete package.' }
$identityText = & $executable --build-identity | Out-String -Stream
if ($LASTEXITCODE -ne 0) { throw 'Encoder cannot load. Keep the complete bin folder and all subfolders.' }
$identity = ($identityText -join "`n") | ConvertFrom-Json
if ($identity.gitCommit -cne $expected.gitCommit -or $identity.applicationName -cne 'PixelBridgeEncoder')
{
    throw 'Unexpected Encoder build identity.'
}
if ($CheckOnly)
{
    Write-Host ('PASS: complete Encoder package loads; commit ' + $identity.gitCommit)
    Write-Host 'Fresh diagnostic cadence: 5 Hz. No source generated, run directory created or broadcast started.'
    exit 0
}

$running = @(Get-Process -Name PixelBridgeEncoder -ErrorAction SilentlyContinue)
if ($running.Count -ne 0) { throw 'Stop every existing PixelBridgeEncoder manually before starting this diagnostic.' }
$runId = [Guid]::NewGuid().ToString('N')
$runRoot = Join-Path $root ('runs\diagnostic-fresh-5hz-' + $runId)
if (Test-Path -LiteralPath $runRoot) { throw 'Create-only diagnostic run directory already exists.' }
New-Item -ItemType Directory -Path $runRoot | Out-Null
$source = Join-Path $runRoot ('g21-diagnostic-fresh-5hz-' + $runId + '.bin')
$random = [Security.Cryptography.RandomNumberGenerator]::Create()
$file = [IO.File]::Open($source, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try
{
    $buffer = New-Object byte[] (1MB)
    $random.GetBytes($buffer)
    $file.Write($buffer, 0, $buffer.Length)
    $file.Flush($true)
}
finally { $file.Dispose(); $random.Dispose() }
$sourceItem = Get-Item -LiteralPath $source
if ($sourceItem.PSIsContainer -or $sourceItem.Length -ne 1MB -or ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint))
{
    throw 'Fresh source is not a regular 1 MiB file.'
}

# Keep the newly hashed source immutable for the entire Encoder invocation.
$sourceLease = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try
{
    $sourceHash = (Get-FileHash -InputStream $sourceLease -Algorithm SHA256).Hash.ToLowerInvariant()
    $reportPath = Join-Path $runRoot 'encoder-report.json'
    $journalPath = Join-Path $runRoot 'encoder-evidence.jsonl'
    $command = @('--headless-broadcast', '--source', $source, '--profile', 'unified', '--channel', 'remote', '--remote-provider', 'UnknownRemoteLink',
        '--single-monitor-fullscreen', 'primary', '--logical-fps', '5', '--seconds', '600', '--manual-stop', '--run-id', $runId,
        '--report', $reportPath, '--journal', $journalPath)
    $manifest = [ordered]@{
        schema = 'PixelBridge.G21.FreshFiveHzDiagnostic.1'; stage = 'diagnostic-fresh-5hz'; runId = $runId
        sourceName = [IO.Path]::GetFileName($source); bytes = $sourceItem.Length; sha256 = $sourceHash
        generator = 'Remote-host OS CSPRNG; bounded 1 MiB buffer; new source because the original fixture was deleted'
        sourceTransferredToDecoder = $false; configuredLogicalFps = 5; hardMaximumBroadcastSeconds = 600
        originalFixtureAvailable = $false; strictSameSourceComparison = $false; G21AcceptanceRun = $false
        presentationMode = 'Primary monitor borderless fullscreen; exact 1920x1080 canvas centered at 1:1 in neutral matte'
        comparison = 'Fresh-source cadence diagnostic only; cannot replace the deleted same-source comparison or default 15 Hz G21 gate'
        build = $identity; encoderSha256 = $actualHash; arguments = $command
    }
    WriteNewText (Join-Path $runRoot 'source-manifest.json') ($manifest | ConvertTo-Json -Depth 5)
    Write-Host ('RUN: ' + $runRoot)
    Write-Host ('NEW SOURCE SHA256: ' + $sourceHash)
    Write-Host 'DIAGNOSTIC: fresh 1 MiB CSPRNG source at a requested logical cadence of 5 Hz.'
    Write-Host 'Keep all four finder boxes and the entire canvas visible on the LOCAL RIGHT screen.'
    Write-Host 'Do not send the .bin source. When asked to stop, focus THIS console and press Enter or Q.'
    Write-Host 'The 600-second sender safety limit remains. Deadline exit is not receiver success.'
    & $executable @command | Out-Host
    $encoderExit = $LASTEXITCODE
    WriteNewText (Join-Path $runRoot 'process-exit.json') (@{ encoderExit = $encoderExit; finishedUtc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json)
    Write-Host ('Encoder stopped with exit ' + $encoderExit + '. Preserve ALL evidence.')
    Write-Host 'Share this run source-manifest.json and encoder-report.json after the run; never the source .bin.'
    exit $encoderExit
}
finally { $sourceLease.Dispose() }

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

$expectedBuild = ReadSmallJson (Join-Path $root 'expected-build.json')
$expectedSource = ReadSmallJson (Join-Path $root 'expected-source-5hz.json')
if ($expectedSource.originalRunId -cnotmatch '^[0-9a-f]{32}$' -or $expectedSource.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $expectedSource.bytes -ne 1MB -or $expectedSource.encoderCommit -ne $expectedBuild.gitCommit -or
    $expectedSource.encoderSha256 -ne $expectedBuild.encoderSha256)
{
    throw 'Expected same-source metadata does not match this 1 MiB diagnostic kit.'
}
$sourceName = 'g21-smoke-' + $expectedSource.originalRunId + '.bin'
if ($expectedSource.sourceName -cne $sourceName) { throw 'Expected source basename does not match its original run.' }
$originalRunRoot = Join-Path $root ('runs\smoke-' + $expectedSource.originalRunId)
$source = Join-Path $originalRunRoot $sourceName
$sourceItem = Get-Item -LiteralPath $source
if ($sourceItem.PSIsContainer -or $sourceItem.Length -ne 1MB -or ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint))
{
    throw 'The original 1 MiB source is missing or changed; no replacement source will be generated.'
}

# Deny source writes and deletion continuously from hashing through Encoder exit.
$sourceLease = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try
{
    $sourceHash = (Get-FileHash -InputStream $sourceLease -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($sourceHash -cne $expectedSource.sha256) { throw 'Original source SHA256 mismatch; refusing to generate or substitute data.' }
    $executable = Join-Path $root 'bin\PixelBridgeEncoder.exe'
    $actualHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedBuild.encoderSha256) { throw 'Encoder executable hash mismatch; preserve the original kit.' }
    $identityText = & $executable --build-identity
    if ($LASTEXITCODE -ne 0) { throw 'Encoder cannot load from the original kit.' }
    $identity = ($identityText -join "`n") | ConvertFrom-Json
    if ($identity.gitCommit -ne $expectedBuild.gitCommit -or $identity.applicationName -ne 'PixelBridgeEncoder')
    {
        throw 'Unexpected Encoder build identity.'
    }
    if ($CheckOnly)
    {
        Write-Host ('PASS: original source verified; SHA256 ' + $sourceHash)
        Write-Host ('PASS: Encoder loads; commit ' + $identity.gitCommit)
        Write-Host 'Fixed diagnostic cadence: 5 Hz. No source generated, run directory created or broadcast started.'
        exit 0
    }
    $running = @(Get-Process -Name PixelBridgeEncoder -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { throw 'Stop the existing Encoder manually before starting this diagnostic.' }
    $runId = [Guid]::NewGuid().ToString('N')
    $runRoot = Join-Path $root ('runs\diagnostic-5hz-' + $runId)
    if (Test-Path -LiteralPath $runRoot) { throw 'Create-only diagnostic run directory already exists.' }
    New-Item -ItemType Directory -Path $runRoot | Out-Null
    $reportPath = Join-Path $runRoot 'encoder-report.json'
    $journalPath = Join-Path $runRoot 'encoder-evidence.jsonl'
    $command = @('--headless-broadcast', '--source', $source, '--profile', 'unified', '--channel', 'remote', '--remote-provider', 'UnknownRemoteLink',
        '--single-monitor-fullscreen', 'primary', '--logical-fps', '5', '--seconds', '600', '--manual-stop', '--run-id', $runId,
        '--report', $reportPath, '--journal', $journalPath)
    $manifest = [ordered]@{
        schema = 'PixelBridge.G21.SameSourceDiagnostic.1'; stage = 'diagnostic-5hz'; runId = $runId
        originalRunId = $expectedSource.originalRunId; sourceName = $sourceName; bytes = $sourceItem.Length; sha256 = $sourceHash
        generator = 'Reuse existing remote source; no new payload generated; read lease held throughout run'
        sourceTransferredToDecoder = $false; configuredLogicalFps = 5; hardMaximumBroadcastSeconds = 600
        G21AcceptanceRun = $false; comparison = 'Cadence diagnostic only; does not satisfy the default 15 Hz G21 gate'
        presentationMode = 'Primary monitor borderless fullscreen; exact 1920x1080 canvas centered at 1:1 in neutral matte'
        build = $identity; encoderSha256 = $actualHash; arguments = $command
    }
    WriteNewText (Join-Path $runRoot 'source-manifest.json') ($manifest | ConvertTo-Json -Depth 5)
    Write-Host ('RUN: ' + $runRoot)
    Write-Host ('SAME SOURCE SHA256: ' + $sourceHash)
    Write-Host 'DIAGNOSTIC: fixed 5 Hz; all other transmission settings unchanged.'
    Write-Host 'Keep all four finder boxes and the entire canvas visible on the LOCAL RIGHT screen.'
    Write-Host 'Do not regenerate or send the .bin source. When asked to stop, focus THIS console and press Enter or Q.'
    Write-Host 'The original 600-second sender safety limit remains. Deadline exit is not receiver success.'
    & $executable @command
    $encoderExit = $LASTEXITCODE
    WriteNewText (Join-Path $runRoot 'process-exit.json') (@{ encoderExit = $encoderExit; finishedUtc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json)
    Write-Host ('Encoder stopped with exit ' + $encoderExit + '. Preserve ALL evidence.')
    Write-Host 'Share this diagnostic run source-manifest.json and encoder-report.json after the run; never the source .bin.'
    exit $encoderExit
}
finally { $sourceLease.Dispose() }

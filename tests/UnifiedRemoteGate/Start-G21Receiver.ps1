param(
    [ValidateSet('smoke', 'full')][string]$Stage = 'smoke',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$executable = Join-Path $root 'bin\PBUnifiedRemoteGate.exe'
$expected = Get-Content -LiteralPath (Join-Path $root 'expected-build.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ine $expected.gateSha256) { throw 'Frozen Gate executable hash mismatch.' }
$identity = (& $executable --build-identity) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $identity.gitCommit -cne $expected.gitCommit) { throw 'Gate build identity mismatch.' }
if ($CheckOnly)
{
    & $executable --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Gate policy self-test failed.' }
    Write-Host ('PASS: Gate load/identity/self-test; ' + $identity.gitCommit + '; no capture, window, source or run created.')
    exit 0
}
if (@(Get-Process PixelBridgeEncoder,PixelBridgeDecoder,PBUnifiedRemoteGate -ErrorAction SilentlyContinue).Count -ne 0) { throw 'An existing PixelBridge process must finish before this fresh run.' }
$run = Join-Path $root ('runs\' + $Stage + '-' + [Guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $run) { throw 'Create-only receiver root already exists.' }
[void](New-Item -ItemType Directory -Path $run)
$seconds = if ($Stage -eq 'smoke') { 600 } else { 1800 }
& $executable --preflight (Join-Path $run 'preflight')
if ($LASTEXITCODE -ne 0) { throw 'Current right-monitor preflight rejected; capture not started.' }
$receiver = Join-Path $run 'receiver'
$arguments = '--receive "' + $receiver + '" ' + $seconds.ToString([Globalization.CultureInfo]::InvariantCulture)
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $run 'launch.json'), (@{
    gitCommit = $identity.gitCommit; executable = $executable; arguments = @('--receive', $receiver, $seconds)
    sourceOrOracleProvided = $false; authority = 'Pending remote field scene; this entry alone is not a field pass'
    monitorPolicy = 'Re-enumerate entire physical right monitor; protect left monitor; no input automation'
} | ConvertTo-Json -Depth 5), $utf8)
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $executable
$start.Arguments = $arguments
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object Diagnostics.Process
$process.StartInfo = $start
$forced = $false
$started = $false
$failureReason = $null
$wrapperFailure = $null
$memoryWriter = $null
$peakWorkingSet = 0L
$peakPrivate = 0L
$samples = 0L
try
{
    if (-not $process.Start()) { throw 'Gate process did not start.' }
    $started = $true
    $memoryStream = [IO.File]::Open((Join-Path $run 'memory-samples.jsonl'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $memoryWriter = New-Object IO.StreamWriter($memoryStream, $utf8)
    $memoryWriter.AutoFlush = $true
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Write-Host ('RECEIVER RUN: ' + $run)
    Write-Host 'Receiver is running first; now start the matching remote sender and keep its full canvas visible on the local RIGHT monitor.'
    while (-not $process.WaitForExit(1000))
    {
        $process.Refresh()
        $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.WorkingSet64)
        $peakPrivate = [Math]::Max($peakPrivate, $process.PrivateMemorySize64)
        $samples++
        if ($samples -gt ($seconds + 31)) { $failureReason = 'Bounded memory sample limit exceeded.' }
        else
        {
            $memoryWriter.WriteLine((@{ elapsedMilliseconds = $clock.ElapsedMilliseconds; workingSetBytes = $process.WorkingSet64; privateBytes = $process.PrivateMemorySize64 } | ConvertTo-Json -Compress))
        }
        $samplePath = Join-Path $receiver 'samples.jsonl'
        if (Test-Path -LiteralPath $samplePath)
        {
            # Read only the bounded final report line, not the complete historical log.
            try { $sampleText = Get-Content -LiteralPath $samplePath -Tail 1 }
            catch { $sampleText = $null }
            if ($sampleText)
            {
                try { $sample = $sampleText | ConvertFrom-Json }
                catch { $sample = $null } # A concurrent partial line is retried next second.
                if ($sample)
                {
                    if (($sample.unifiedTelemetry.observationAvailable -eq $true -and $sample.unifiedTelemetry.frameCoverageComplete -eq $false) -or $sample.unifiedTelemetry.counterOverflow -eq $true)
                    {
                        $failureReason = 'Incomplete observed-frame coverage or telemetry counter overflow.'
                    }
                    foreach ($name in @('outerResourceRejections', 'outerConflictRejections', 'outerDeferredResourceBusyCount', 'outerFecQuotaExceededCount', 'receiverResourcePolicyRejectedCount', 'receiverControlRejectedByResourcePolicyCount', 'outerOrphanDroppedByQuotaCount', 'outerOrphanResourceExhaustedCount', 'outerOrphanConflictRejectionCount'))
                    {
                        if ($sample.remoteGate.$name -gt 0) { $failureReason = 'Irreversible receiver resource/conflict gate: ' + $name; break }
                    }
                    if ($failureReason)
                    {
                        [IO.File]::WriteAllText((Join-Path $run 'receiver-gate-failure-snapshot.json'), $sampleText, $utf8)
                    }
                }
            }
        }
        if ($clock.Elapsed.TotalSeconds -gt ($seconds + 30)) { $failureReason = 'Receiver bounded watchdog expired.' }
        if ($failureReason) { $forced = $true; if (-not $process.HasExited) { $process.Kill() }; break }
    }
    if (-not $process.WaitForExit(10000)) { throw 'Gate did not terminate within the bounded cleanup period.' }
    if (-not $stdout.Wait(10000) -or -not $stderr.Wait(10000)) { throw 'Gate output did not close within the bounded drain period.' }
    [IO.File]::WriteAllText((Join-Path $run 'receiver.stdout.txt'), $stdout.Result, $utf8)
    [IO.File]::WriteAllText((Join-Path $run 'receiver.stderr.txt'), $stderr.Result, $utf8)
    if ($forced -or $process.ExitCode -ne 0) { throw ('Receiver failed; preserve all evidence and do not advance the tier. ' + $failureReason) }
}
catch
{
    $wrapperFailure = $_.ToString()
    throw
}
finally
{
    if ($started -and -not $process.HasExited)
    {
        $forced = $true
        $process.Kill()
        [void]$process.WaitForExit(10000)
    }
    if ($memoryWriter) { $memoryWriter.Dispose() }
    $receiverExit = if ($started -and $process.HasExited) { $process.ExitCode } else { $null }
    [IO.File]::WriteAllText((Join-Path $run 'process-exit.json'), (@{
        receiverExit = $receiverExit; started = $started; forcedTermination = $forced; failureReason = $failureReason
        wrapperFailure = $wrapperFailure; memorySampleIntervalMilliseconds = 1000
        memorySamples = $samples; peakWorkingSetBytes = $peakWorkingSet; peakPrivateBytes = $peakPrivate
        externalDigestAuditStillRequired = $true; remoteSceneAuthorityStillRequired = $true
    } | ConvertTo-Json), $utf8)
    $process.Dispose()
}
Write-Host 'Receiver Gate checks passed. Stop the remote Encoder normally; only then perform independent cross-machine digest audit.'

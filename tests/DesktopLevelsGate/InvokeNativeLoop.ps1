#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Encoder,
    [Parameter(Mandatory)][string]$Decoder,
    [Parameter(Mandatory)][string]$Support,
    [Parameter(Mandatory)][ValidateSet('wgc','dxgi')][string]$Backend,
    [Parameter(Mandatory)][ValidateSet(2,4)][int]$Tile,
    [Parameter(Mandatory)][string]$EvidenceRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-NewText([string]$Path, [string]$Text)
{
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try
    {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally { $stream.Dispose() }
}

function Start-OwnedProcess([string]$Executable, [string[]]$Arguments)
{
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "Cannot start $Executable" }
    return @{ Process=$process; Output=$process.StandardOutput.ReadToEndAsync(); Error=$process.StandardError.ReadToEndAsync() }
}

$runName = "{0}-{1}-{2}x{2}-{3}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'),$Backend,$Tile,([Guid]::NewGuid().ToString('N'))
$directory = [IO.Path]::GetFullPath((Join-Path $EvidenceRoot $runName))
[IO.Directory]::CreateDirectory($directory) | Out-Null
$sender = $null
$receiver = $null
$result = [ordered]@{ Gate='FAIL'; Backend=$Backend; Candidate="desktop-levels-${Tile}x${Tile}"; CaptureSeconds=30; RequiredVerifiedFrames=16; RequiredVerifiedPhases=65535; Evidence=$directory }
$commands = [Collections.Generic.List[object]]::new()
$visibility = [Collections.Generic.List[object]]::new()
$exitCode = 1
try
{
    $environmentText = (& $Support --environment | Out-String)
    $environmentExit = $LASTEXITCODE
    Write-NewText (Join-Path $directory 'environment.json') $environmentText
    if ($environmentExit -ne 0) { throw 'Real SDR / complete 1920x1080 physical desktop precondition failed (not skipped)' }
    $environment = $environmentText | ConvertFrom-Json
    if (-not $environment.SDR -or $environment.hdr) { throw 'Signal is not SDR' }
    $encoderLog = Join-Path $directory 'encoder.jsonl'
    $decoderLog = Join-Path $directory 'decoder.jsonl'
    $encoderArguments = @('--visual',"desktop-levels-${Tile}x${Tile}",'--frames','80','--telemetry',$encoderLog)
    $commands.Add(@{Executable=$Encoder;Arguments=$encoderArguments})
    # Finite sender lifetime also bounds cleanup if the parent is terminated.
    $sender = Start-OwnedProcess $Encoder $encoderArguments
    $window = $null
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(8)
    while ([DateTime]::UtcNow -lt $windowDeadline -and -not $sender.Process.HasExited)
    {
        $windowText = (& $Support --window $sender.Process.Id | Out-String)
        if ($LASTEXITCODE -eq 0)
        {
            $window = $windowText | ConvertFrom-Json
            if ($window.found -and $window.visible) { break }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $window -or -not $window.visible) { throw 'Test-owned Encoder DataWindow was not fully visible' }
    if (($window.roi -join ',') -ne ($environment.roi -join ',')) { throw 'Encoder physical ROI does not match the validated monitor region' }
    $visibility.Add($window)
    $decoderArguments = @('--capture-desktop-levels','--backend',$Backend,'--seconds','30','--roi') + @($window.roi | ForEach-Object { [string]$_ }) + @('--telemetry',$decoderLog)
    $commands.Add(@{Executable=$Decoder;Arguments=$decoderArguments})
    $receiver = Start-OwnedProcess $Decoder $decoderArguments
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (-not $receiver.Process.HasExited)
    {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Decoder exceeded bounded capture/init/shutdown duration' }
        if ($sender.Process.HasExited) { throw 'Encoder exited before the full 30-second capture window' }
        $proofText = (& $Support --window $sender.Process.Id | Out-String)
        if ($LASTEXITCODE -ne 0) { throw 'Physical data region became occluded or unavailable' }
        $proof = $proofText | ConvertFrom-Json
        if (-not $proof.visible -or ($proof.roi -join ',') -ne ($window.roi -join ',')) { throw 'Physical region moved or lost 1:1 visibility' }
        $visibility.Add($proof)
        Start-Sleep -Milliseconds 400
    }
    $receiver.Process.WaitForExit()
    if (-not $sender.Process.WaitForExit(18000)) { throw 'Finite Encoder did not finish its 80 submissions' }
    $result['EncoderExit'] = $sender.Process.ExitCode
    $result['DecoderExit'] = $receiver.Process.ExitCode
    $rows = @(Get-Content -LiteralPath $decoderLog | ForEach-Object { $_ | ConvertFrom-Json })
    $finals = @($rows | Where-Object event -eq 'capture-final')
    if ($finals.Count -ne 1) { throw 'Exactly one authoritative final snapshot is required' }
    $final = $finals[0]
    if ($final.elapsedMilliseconds -lt 30000 -or -not $final.shutdownComplete -or $final.deferredCleanup -or -not $final.readback.workerStopped) { throw 'Capture duration/shutdown contract incomplete' }
    if ($final.hdr -or $final.colorSpace -ne 0) { throw 'Final capture output is HDR/unknown instead of SDR' }
    if ($final.readback.processingReservedBytes -ne 16777216) { throw 'Processor reservation was not included in readback accounting' }
    if ($final.visual.diagnosticQueueDrops -ne 0 -or $final.staleDiagnosticEvents -ne 0) { throw 'Observation evidence lost before JSONL serialization' }
    if ($final.desktopLevels.statisticsFailures -ne 0 -or $final.visual.identityConflicts -ne 0) { throw 'Statistics overflow or profile/identity conflict' }
    $candidateIndex = if ($Tile -eq 2) { 0 } else { 1 }
    $candidate = $final.desktopLevels.candidates[$candidateIndex]
    $metrics = $candidate.metrics
    # Keep measured failures and zero-denominator nulls in failed reports too.
    $result['Metrics'] = $metrics
    $result['GeometryErasures'] = $candidate.geometryErasures
    $result['PilotErasures'] = $candidate.pilotErasures
    $result['UnrecognizedBootstrap'] = $final.desktopLevels.unrecognizedBootstrap
    $result['Duplicates'] = $candidate.duplicates
    $result['CaptureDrops'] = $final.droppedFrames
    $result['ReadbackDrops'] = $final.readback.drops
    if ($sender.Process.ExitCode -ne 0 -or $receiver.Process.ExitCode -ne 0)
    {
        throw "Native entries failed: encoder=$($sender.Process.ExitCode) decoder=$($receiver.Process.ExitCode)"
    }
    $other = $final.desktopLevels.candidates[1 - $candidateIndex].metrics
    if ($other.frames -ne 0) { throw 'A different candidate was silently admitted' }
    if ($metrics.falseAcceptedCodewords -ne 0) { throw 'CRC-valid non-truth data was observed' }
    if ($metrics.verifiedFrames -lt 16 -or $metrics.verifiedPhases -ne 65535) { throw "Insufficient complete sequence/phase evidence: frames=$($metrics.verifiedFrames) phases=$($metrics.verifiedPhases)" }
    if ($null -eq $metrics.PreFecBER -or $null -eq $metrics.PreFecFER -or $null -eq $metrics.PostFecFER) { throw 'Nonempty denominators must have defined metrics' }

    # Recompute numerators and denominators from ALL admitted observations, not
    # only successes. Capture/geometry/pilot erasures and duplicates stay out.
    [UInt64]$frames = 0; [UInt64]$bits = 0; [UInt64]$errors = 0
    [UInt64]$preFailed = 0; [UInt64]$postFailed = 0; [UInt64]$verified = 0
    [UInt64]$phases = 0
    $identities = [Collections.Generic.HashSet[string]]::new()
    foreach ($row in @($rows | Where-Object event -eq 'desktop-levels-observation'))
    {
        if ($row.hdr -or $row.signalEncoding -eq 0) { throw 'Unsupported signal reached an observation during the SDR run' }
        if ($row.disposition -notin @('Accepted','PostFecFailure')) { continue }
        if (-not $row.evaluation.evaluated -or $null -eq $row.identity) { throw 'Admitted frame lacks complete FEC evaluation/identity' }
        if (-not $identities.Add("$($row.domain.sourceId):$($row.domain.captureEpoch):$($row.identity.SessionTag):$($row.identity.FrameSequence)")) { throw 'Duplicate identity entered an independent denominator' }
        $frames++; $bits += $row.evaluation.comparedCodedBits; $errors += $row.evaluation.erroneousCodedBits
        if ($row.evaluation.erroneousCodedBits -ne 0) { $preFailed++ }
        if ($row.evaluation.verified)
        {
            $verified++
            $phases = $phases -bor (1 -shl ([UInt64]$row.identity.FrameSequence % 16))
        }
        else { $postFailed++ }
        if ($row.disposition -eq 'Accepted' -and (-not $row.evaluation.verified -or $row.evaluation.falseAcceptedCodewords -ne 0)) { throw 'Accepted payload is not exact truth' }
    }
    if ($frames -ne $metrics.frames -or $bits -ne $metrics.comparedCodedBits -or $errors -ne $metrics.erroneousCodedBits -or
        $preFailed -ne $metrics.preFecFailedFrames -or $postFailed -ne $metrics.postFecFailedFrames -or $verified -ne $metrics.verifiedFrames -or $phases -ne $metrics.verifiedPhases)
    { throw 'Raw JSONL recomputation differs from the authoritative measurement summary' }
    $result.Gate = 'PASS'
    $result['RawNumeratorsVerified'] = $true
    $exitCode = 0
}
catch
{
    $result['Error'] = $_.Exception.Message
}
finally
{
    foreach ($owned in @($receiver,$sender))
    {
        if ($null -ne $owned -and -not $owned.Process.HasExited)
        {
            # Only test-owned processes/windows are stopped; no external windows,
            # cursor, HDR, resolution, DPI or monitor mode are changed.
            if ($owned -eq $sender) { & $Support --close $owned.Process.Id | Out-Null }
            if (-not $owned.Process.WaitForExit(3000)) { $owned.Process.Kill(); $owned.Process.WaitForExit() }
        }
    }
    foreach ($pair in @(@('encoder',$sender),@('decoder',$receiver)))
    {
        if ($null -ne $pair[1])
        {
            Write-NewText (Join-Path $directory ($pair[0] + '-stdout.txt')) ($pair[1].Output.GetAwaiter().GetResult())
            Write-NewText (Join-Path $directory ($pair[0] + '-stderr.txt')) ($pair[1].Error.GetAwaiter().GetResult())
            $pair[1].Process.Dispose()
        }
    }
    Write-NewText (Join-Path $directory 'commands.json') ($commands | ConvertTo-Json -Depth 8)
    Write-NewText (Join-Path $directory 'visibility.json') ($visibility | ConvertTo-Json -Depth 6)
    $json = $result | ConvertTo-Json -Depth 12
    Write-NewText (Join-Path $directory 'report.json') $json
    Write-Output $json
}
exit $exitCode

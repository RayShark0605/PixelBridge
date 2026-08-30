#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Encoder,
    [Parameter(Mandatory)][string]$Decoder,
    [Parameter(Mandatory)][string]$Support,
    [Parameter(Mandatory)][ValidateSet('wgc','dxgi')][string]$Backend,
    [Parameter(Mandatory)][ValidateSet('desktop-levels-2x2','desktop-levels-4x4','shape-chroma')][string]$Candidate,
    [Parameter(Mandatory)][string]$EvidenceRoot,
    [Parameter(Mandatory)][int]$MonitorOriginX,
    [Parameter(Mandatory)][int]$MonitorOriginY
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'NativeLoopPolicy.ps1')

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

$runName = "{0}-{1}-{2}-{3}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'),$Backend,$Candidate,([Guid]::NewGuid().ToString('N'))
$directory = [IO.Path]::GetFullPath((Join-Path $EvidenceRoot $runName))
[IO.Directory]::CreateDirectory($directory) | Out-Null
$sender = $null
$receiver = $null
$nativeModeRaw = $env:PB_DESKTOP_LEVELS_NATIVE_MODE
$nativeMode = if ($null -ne $nativeModeRaw) { $nativeModeRaw.ToLower() } else { 'release' }
$policy = Get-DesktopLevelsNativeLoopPolicy $nativeMode
$minVerifiedFrames = $policy.RequiredVerifiedFrames
$minVerifiedPhases = $policy.RequiredVerifiedPhases
$result = [ordered]@{ Gate='FAIL'; Backend=$Backend; Candidate=$Candidate; CaptureSeconds=$policy.CaptureSeconds; NativeMode=$nativeMode; RequiredVerifiedFrames=$minVerifiedFrames; RequiredVerifiedPhases=$minVerifiedPhases; MonitorOrigin=@($MonitorOriginX,$MonitorOriginY); Evidence=$directory }
$commands = [Collections.Generic.List[object]]::new()
$visibility = [Collections.Generic.List[object]]::new()
$exitCode = 1
try
{
    $environmentArguments = @('--environment',[string]$MonitorOriginX,[string]$MonitorOriginY)
    $environmentText = (& $Support @environmentArguments | Out-String)
    $environmentExit = $LASTEXITCODE
    $commands.Add(@{Executable=$Support;Arguments=$environmentArguments;ExitCode=$environmentExit})
    Write-NewText (Join-Path $directory 'environment.json') $environmentText
    if ($environmentExit -ne 0) { throw 'Real SDR / complete 1920x1080 physical desktop precondition failed (not skipped)' }
    $environment = $environmentText | ConvertFrom-Json
    if (-not $environment.SDR -or $environment.hdr) { throw 'Signal is not SDR' }
    $expectedRoi = @([Int64]$MonitorOriginX,[Int64]$MonitorOriginY,([Int64]$MonitorOriginX + 1920),([Int64]$MonitorOriginY + 1080))
    if ((@($environment.roi) -join ',') -cne ($expectedRoi -join ',')) { throw 'Environment probe did not preserve the explicit physical test origin' }
    # Both backends deliver cursor-free pixels (WGC disables cursor capture;
    # DXGI duplication excludes the pointer from frames). The pointer-in-ROI
    # guard is still a conservative fail-closed environment contract: an
    # operator active inside the fixture region would otherwise degrade into
    # an obscure late failure after the full 30-second capture.
    if ($Backend -eq 'dxgi' -and $environment.pointerInsideRoi) { throw 'Pointer is inside the capture ROI; move it outside the region and re-run the gate' }
    $encoderLog = Join-Path $directory 'encoder.jsonl'
    $decoderLog = Join-Path $directory 'decoder.jsonl'
    # The finite sender lifetime exceeds the receiver's bounded deadline, not
    # merely its requested capture duration. CPU diagnostic work can make a
    # nominal 30-second run finish several seconds late. The sender is closed
    # naturally after receiver shutdown; this keeps a zero exit status and a
    # complete sender telemetry record. Same-phase copies remain 10.24 s apart.
    $encoderArguments = @('--visual',$Candidate,'--origin',[string]$MonitorOriginX,[string]$MonitorOriginY,
        '--frames',"$($policy.SenderFrames)",'--sequence-interval-ms',"$($policy.SequenceIntervalMilliseconds)",'--telemetry',$encoderLog)
    $commands.Add(@{Executable=$Encoder;Arguments=$encoderArguments})
    # Finite sender lifetime also bounds cleanup if the parent is terminated.
    $sender = Start-OwnedProcess $Encoder $encoderArguments
    $window = $null
    $lastWindowText = ''
    $lastWindowExit = $null
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(8)
    while ([DateTime]::UtcNow -lt $windowDeadline -and -not $sender.Process.HasExited)
    {
        $windowText = (& $Support --window $sender.Process.Id | Out-String)
        $lastWindowText = $windowText.Trim()
        $lastWindowExit = $LASTEXITCODE
        if ($lastWindowExit -eq 0)
        {
            $window = $windowText | ConvertFrom-Json
            if ($window.found -and $window.visible) { break }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $window -or -not $window.visible)
    {
        throw "Test-owned Encoder DataWindow was not fully visible (probeExit=$lastWindowExit; probe=$lastWindowText; encoderExited=$($sender.Process.HasExited))"
    }
    if (($window.roi -join ',') -ne ($environment.roi -join ',')) { throw 'Encoder physical ROI does not match the validated monitor region' }
    $visibility.Add($window)
    $decoderMode = if ($Candidate -eq 'shape-chroma') { '--capture-shape-chroma' } else { '--capture-desktop-levels' }
    $decoderArguments = @($decoderMode,'--backend',$Backend,'--seconds',"$($policy.CaptureSeconds)",'--roi') + @($window.roi | ForEach-Object { [string]$_ }) + @('--telemetry',$decoderLog)
    $commands.Add(@{Executable=$Decoder;Arguments=$decoderArguments})
    $receiver = Start-OwnedProcess $Decoder $decoderArguments
    $deadline = [DateTime]::UtcNow.AddSeconds($policy.ReceiverDeadlineSeconds)
    while (-not $receiver.Process.HasExited)
    {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Decoder exceeded bounded capture/init/shutdown duration' }
        if ($sender.Process.HasExited) { throw 'Encoder exited before the full 30-second capture window' }
        $proofText = (& $Support --window $sender.Process.Id | Out-String)
        if ($LASTEXITCODE -ne 0) { throw 'Physical data region became occluded or unavailable' }
        $proof = $proofText | ConvertFrom-Json
        if (-not $proof.visible -or ($proof.roi -join ',') -ne ($window.roi -join ',')) { throw 'Physical region moved or lost 1:1 visibility' }
        if ($Backend -eq 'dxgi' -and $proof.pointerInsideRoi) { throw 'Pointer entered the capture ROI during the run; move it outside and re-run the gate' }
        $visibility.Add($proof)
        Start-Sleep -Milliseconds 400
    }
    $receiver.Process.WaitForExit()
    if (-not $sender.Process.WaitForExit($policy.SenderShutdownMilliseconds)) { throw 'Finite Encoder did not finish its bounded submissions' }
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
    $physical = if ($Candidate -eq 'shape-chroma') { $final.shapeChroma } else { $final.desktopLevels }
    if ($physical.statisticsFailures -ne 0 -or $final.visual.identityConflicts -ne 0) { throw 'Statistics overflow or profile/identity conflict' }
    $candidateIndex = if ($Candidate -eq 'desktop-levels-2x2') { 0 } elseif ($Candidate -eq 'desktop-levels-4x4') { 1 } else { -1 }
    $candidateSummary = if ($Candidate -eq 'shape-chroma') { $physical.candidate } else { $physical.candidates[$candidateIndex] }
    $metrics = $candidateSummary.metrics
    # Keep measured failures and zero-denominator nulls in failed reports too.
    $result['Metrics'] = $metrics
    $result['GeometryErasures'] = $candidateSummary.geometryErasures
    $result['PilotErasures'] = $candidateSummary.pilotErasures
    $result['UnrecognizedBootstrap'] = $physical.unrecognizedBootstrap
    $result['Duplicates'] = $candidateSummary.duplicates
    $result['CaptureDrops'] = $final.droppedFrames
    $result['ReadbackDrops'] = $final.readback.drops
    if ($sender.Process.ExitCode -ne 0 -or $receiver.Process.ExitCode -ne 0)
    {
        throw "Native entries failed: encoder=$($sender.Process.ExitCode) decoder=$($receiver.Process.ExitCode)"
    }
    if ($candidateIndex -ge 0)
    {
        $other = $physical.candidates[1 - $candidateIndex].metrics
        if ($other.frames -ne 0) { throw 'A different candidate was silently admitted' }
    }
    if ($metrics.falseAcceptedCodewords -ne 0) { throw 'CRC-valid non-truth data was observed' }
    $verifiedPhaseCount = 0
    [UInt64]$verifiedPhaseMask = $metrics.verifiedPhases
    while ([UInt64]$verifiedPhaseMask -ne 0)
    {
        $verifiedPhaseCount += [int]([UInt64]$verifiedPhaseMask -band 1)
        $verifiedPhaseMask = [UInt64]([UInt64]$verifiedPhaseMask -shr 1)
    }
    if ($metrics.verifiedFrames -lt $minVerifiedFrames -or $verifiedPhaseCount -lt $minVerifiedPhases) { throw "Insufficient sequence/phase evidence: frames=$($metrics.verifiedFrames) (min $minVerifiedFrames), phases=$verifiedPhaseCount/16 (min $minVerifiedPhases)" }
    if ($null -eq $metrics.PreFecBER -or $null -eq $metrics.PreFecFER -or $null -eq $metrics.PostFecFER) { throw 'Nonempty denominators must have defined metrics' }

    # Recompute numerators and denominators from ALL admitted observations, not
    # only successes. Capture/geometry/pilot erasures and duplicates stay out.
    [UInt64]$frames = 0; [UInt64]$bits = 0; [UInt64]$errors = 0
    [UInt64]$preFailed = 0; [UInt64]$postFailed = 0; [UInt64]$verified = 0
    [UInt64]$phases = 0
    $identities = [Collections.Generic.HashSet[string]]::new()
    $observationEvent = if ($Candidate -eq 'shape-chroma') { 'shape-chroma-observation' } else { 'desktop-levels-observation' }
    foreach ($row in @($rows | Where-Object event -eq $observationEvent))
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
    # Disclosure only: whether the probe actually established WS_EX_TOPMOST.
    # A local security hook may silently strip the attribute while SetWindowPos
    # still reports success. The z-order occlusion scan and the in-band marker
    # verification remain the authoritative visibility evidence.
    $topmostRaisedApplied = $null
    foreach ($entry in $visibility)
    {
        if ($entry.PSObject.Properties.Name -contains 'raisedApplied')
        {
            if ($null -eq $topmostRaisedApplied) { $topmostRaisedApplied = [bool]$entry.raisedApplied }
            elseif (-not $entry.raisedApplied) { $topmostRaisedApplied = $false }
        }
    }
    if ($null -ne $topmostRaisedApplied) { $result['TopmostRaisedApplied'] = $topmostRaisedApplied }
    Write-NewText (Join-Path $directory 'commands.json') ($commands | ConvertTo-Json -Depth 8)
    Write-NewText (Join-Path $directory 'visibility.json') ($visibility | ConvertTo-Json -Depth 6)
    $json = $result | ConvertTo-Json -Depth 12
    Write-NewText (Join-Path $directory 'report.json') $json
    Write-Output $json
}
exit $exitCode

#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GateExecutable,
    [Parameter(Mandatory)][string]$Support,
    [Parameter(Mandatory)][ValidateSet('wgc','dxgi')][string]$Backend,
    [Parameter(Mandatory)][ValidateSet('desktop-levels-2x2','shape-chroma')][string]$Profile,
    [Parameter(Mandatory)][string]$EvidenceRoot,
    [Parameter(Mandatory)][ValidateSet('release','asan')][string]$NativeMode,
    [UInt32]$SoakSeconds = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'GateJson.ps1')
. (Join-Path $PSScriptRoot 'Phase1FileGatePolicy.ps1')
. (Join-Path $PSScriptRoot 'Phase1FileResources.ps1')

function Write-NewText([string]$Path, [string]$Text)
{
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try
    {
        $file.Write($bytes, 0, $bytes.Length)
        $file.Flush($true)
    }
    finally
    {
        $file.Dispose()
    }
}

function Start-OwnedProcess([string]$Executable, [string[]]$Arguments, [string]$WorkingDirectory)
{
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments)
    {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start())
    {
        throw "Cannot start test-owned process: $Executable"
    }
    return [pscustomobject]@{
        Process=$process
        Output=$process.StandardOutput.ReadToEndAsync()
        Error=$process.StandardError.ReadToEndAsync()
    }
}

function Read-LastReceiverSnapshot([string]$Path)
{
    if (-not (Test-Path -LiteralPath $Path))
    {
        return $null
    }
    $last = $null
    foreach ($line in Read-SharedUtf8Lines $Path)
    {
        if ([string]::IsNullOrWhiteSpace($line))
        {
            continue
        }
        try
        {
            $record = $line | ConvertFrom-Json
            if ((Get-JsonEventName $record) -ceq 'phase1-receiver-snapshot')
            {
                $last = $record
            }
        }
        catch
        {
            # The child uses write-through complete JSONL records, but a reader
            # can still race the final WriteFile. The next poll must parse it.
        }
    }
    return $last
}

function Read-UniqueFinal([string]$Path, [string]$Event)
{
    if (-not (Test-Path -LiteralPath $Path))
    {
        throw "Missing telemetry file: $Path"
    }
    $records = @()
    foreach ($line in Read-SharedUtf8Lines $Path)
    {
        if ([string]::IsNullOrWhiteSpace($line))
        {
            continue
        }
        $record = $line | ConvertFrom-Json
        if ((Get-JsonEventName $record) -ceq $Event)
        {
            $records += $record
        }
    }
    if ($records.Count -ne 1)
    {
        throw "Exactly one $Event record is required; observed $($records.Count)"
    }
    return $records[0]
}

function Get-ProcessSample([Diagnostics.Stopwatch]$Watch, [object]$Sender, [object]$Receiver)
{
    if ($Sender.Process.HasExited -or $Receiver.Process.HasExited)
    {
        return $null
    }
    try
    {
        $Sender.Process.Refresh()
        $Receiver.Process.Refresh()
        return [pscustomobject]@{
            ElapsedMilliseconds=[Int64]$Watch.ElapsedMilliseconds
            SenderPrivateBytes=[Int64]$Sender.Process.PrivateMemorySize64
            SenderWorkingSetBytes=[Int64]$Sender.Process.WorkingSet64
            SenderHandles=[Int64]$Sender.Process.HandleCount
            SenderCpuMilliseconds=[double]$Sender.Process.TotalProcessorTime.TotalMilliseconds
            ReceiverPrivateBytes=[Int64]$Receiver.Process.PrivateMemorySize64
            ReceiverWorkingSetBytes=[Int64]$Receiver.Process.WorkingSet64
            ReceiverHandles=[Int64]$Receiver.Process.HandleCount
            ReceiverCpuMilliseconds=[double]$Receiver.Process.TotalProcessorTime.TotalMilliseconds
        }
    }
    catch
    {
        # Process properties can become unavailable between HasExited and the
        # individual counter reads. A natural child exit ends sampling; any
        # other query failure remains fatal and preserves the original error.
        if ($Sender.Process.HasExited -or $Receiver.Process.HasExited)
        {
            return $null
        }
        throw
    }
}

$policy = Get-Phase1FileGatePolicy $SoakSeconds $NativeMode
$runName = '{0}-{1}-{2}-soak{3}-{4}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'),$Backend,$Profile,$SoakSeconds,([Guid]::NewGuid().ToString('N'))
$directory = [IO.Path]::GetFullPath((Join-Path $EvidenceRoot $runName))
[IO.Directory]::CreateDirectory($directory) | Out-Null
$sourcePath = Join-Path $directory 'source.bin'
$outputPath = Join-Path $directory 'accepted.bin'
$senderTelemetry = Join-Path $directory 'sender.jsonl'
$receiverTelemetry = Join-Path $directory 'receiver.jsonl'
$sender = $null
$receiver = $null
$commands = [Collections.Generic.List[object]]::new()
$visibility = [Collections.Generic.List[object]]::new()
$resourceSamples = [Collections.Generic.List[object]]::new()
$resizeEvidence = [Collections.Generic.List[object]]::new()
$watch = [Diagnostics.Stopwatch]::StartNew()
$publishedMilliseconds = $null
$result = [ordered]@{
    Gate='FAIL'
    Backend=$Backend
    Profile=$Profile
    NativeMode=$NativeMode
    PerformanceCertification=[bool]$policy.PerformanceCertification
    RequestedSoakSeconds=$SoakSeconds
    Evidence=$directory
}
$exitCode = 1
$resizeApplied = $false
$visibilityMaintained = $true
try
{
    $environmentText = (& $Support --environment 2>&1 | Out-String).Trim()
    $environmentExit = $LASTEXITCODE
    $commands.Add([ordered]@{ Executable=$Support; Arguments=@('--environment'); ExitCode=$environmentExit })
    Write-NewText (Join-Path $directory 'environment.json') $environmentText
    if ($environmentExit -ne 0)
    {
        throw 'Real SDR complete 1920x1080 physical desktop precondition failed'
    }
    $environment = $environmentText | ConvertFrom-Json
    if (-not [bool]$environment.SDR -or [bool]$environment.hdr -or @($environment.roi).Count -ne 4)
    {
        throw 'The physical capture environment is not complete SDR 1920x1080'
    }
    if ($Backend -eq 'dxgi' -and [bool]$environment.pointerInsideRoi)
    {
        throw 'Pointer is inside the DXGI Gate ROI; move it outside and rerun'
    }

    # Arm the real capture path before the finite sender begins its Control
    # preamble. Starting the sender first can miss the preamble and fill the
    # deliberately bounded orphan cache before the next Carousel cycle.
    $receiverArguments = @('--receiver','--profile',$Profile,'--backend',$Backend,'--roi') +
        @($environment.roi | ForEach-Object { [string]$_ }) + @('--output-new',$outputPath,'--telemetry-new',$receiverTelemetry,
        '--timeout-seconds',[string]$policy.ReceiverTimeoutSeconds,'--restart-after-unique',[string]$policy.RestartAfterUniqueFrames,
        '--soak-seconds',[string]$policy.SoakSeconds)
    $commands.Add([ordered]@{ Executable=$GateExecutable; Arguments=$receiverArguments })
    $receiver = Start-OwnedProcess $GateExecutable $receiverArguments $directory
    $deadline = [DateTime]::UtcNow.AddSeconds($policy.ReceiverDeadlineSeconds)
    $receiverReadyDeadline = [DateTime]::UtcNow.AddSeconds($policy.ReceiverReadySeconds)
    $receiverReady = $false
    while ([DateTime]::UtcNow -lt $receiverReadyDeadline -and -not $receiver.Process.HasExited)
    {
        $startupSnapshot = Read-LastReceiverSnapshot $receiverTelemetry
        if ($null -ne $startupSnapshot -and [UInt32]$startupSnapshot.captureState -eq 1)
        {
            $receiverReady = $true
            break
        }
        if ($null -ne $startupSnapshot -and [UInt32]$startupSnapshot.captureState -eq 5)
        {
            throw 'Physical file Receiver entered Failed state before the sender started'
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not $receiverReady)
    {
        throw 'Physical file Receiver did not enter Running state before the bounded startup deadline'
    }

    $senderArguments = @('--sender','--profile',$Profile,'--source-new',$sourcePath,'--telemetry-new',$senderTelemetry,
        '--maximum-sender-seconds',[string]$policy.SenderMaximumSeconds)
    $commands.Add([ordered]@{ Executable=$GateExecutable; Arguments=$senderArguments })
    $sender = Start-OwnedProcess $GateExecutable $senderArguments $directory
    $window = $null
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $lastWindowText = ''
    $lastWindowExit = -1
    while ([DateTime]::UtcNow -lt $windowDeadline -and -not $sender.Process.HasExited)
    {
        $lastWindowText = (& $Support --window $sender.Process.Id 2>&1 | Out-String).Trim()
        $lastWindowExit = $LASTEXITCODE
        if ($lastWindowExit -eq 0)
        {
            $window = $lastWindowText | ConvertFrom-Json
            if ([bool]$window.found -and [bool]$window.visible)
            {
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $window -or -not [bool]$window.visible)
    {
        throw "Test-owned DataWindow is not fully visible (exit=$lastWindowExit probe=$lastWindowText)"
    }
    if ((@($window.roi) -join ',') -cne (@($environment.roi) -join ','))
    {
        throw 'DataWindow client ROI differs from the validated physical ROI'
    }
    $visibility.Add($window)

    $nextProbe = [Int64]0
    $nextSample = [Int64]0
    while (-not $receiver.Process.HasExited)
    {
        if ([DateTime]::UtcNow -ge $deadline)
        {
            throw 'Physical file Receiver exceeded its parent-enforced bounded deadline'
        }
        if ($sender.Process.HasExited)
        {
            throw 'Physical file Sender exited before Receiver completion'
        }
        $elapsed = [Int64]$watch.ElapsedMilliseconds
        if ($elapsed -ge $nextSample)
        {
            $sample = Get-ProcessSample $watch $sender $receiver
            if ($null -ne $sample)
            {
                $resourceSamples.Add($sample)
            }
            $nextSample = $elapsed + [Int64]$policy.ResourceSampleMilliseconds
        }
        if ($null -eq $publishedMilliseconds -and (Test-Path -LiteralPath $outputPath))
        {
            $publishedMilliseconds = [Int64]$elapsed
        }
        $snapshot = Read-LastReceiverSnapshot $receiverTelemetry
        if (-not $resizeApplied -and $null -ne $snapshot -and
            [UInt64]$snapshot.uniqueVisualFrames -ge $policy.ResizeAfterUniqueFrames)
        {
            $shrinkText = (& $Support --resize $sender.Process.Id 1600 900 2>&1 | Out-String).Trim()
            $shrinkExit = $LASTEXITCODE
            $resizeEvidence.Add([ordered]@{ Action='shrink'; ExitCode=$shrinkExit; Output=$shrinkText; ElapsedMilliseconds=$watch.ElapsedMilliseconds })
            if ($shrinkExit -ne 0)
            {
                throw 'Test-owned DataWindow shrink failed'
            }
            Start-Sleep -Milliseconds 750
            $restoreText = (& $Support --resize $sender.Process.Id 1920 1080 2>&1 | Out-String).Trim()
            $restoreExit = $LASTEXITCODE
            $resizeEvidence.Add([ordered]@{ Action='restore'; ExitCode=$restoreExit; Output=$restoreText; ElapsedMilliseconds=$watch.ElapsedMilliseconds })
            if ($restoreExit -ne 0)
            {
                throw 'Test-owned DataWindow restore failed'
            }
            Start-Sleep -Milliseconds 750
            $resizeApplied = $true
            $nextProbe = 0
        }
        if ($resizeApplied -and $elapsed -ge $nextProbe)
        {
            $proofText = (& $Support --window $sender.Process.Id 2>&1 | Out-String).Trim()
            $proofExit = $LASTEXITCODE
            if ($proofExit -ne 0)
            {
                $visibilityMaintained = $false
                throw "Physical DataWindow became occluded or unavailable after recovery: $proofText"
            }
            $proof = $proofText | ConvertFrom-Json
            if (-not [bool]$proof.visible -or (@($proof.roi) -join ',') -cne (@($window.roi) -join ','))
            {
                $visibilityMaintained = $false
                throw 'Recovered DataWindow lost exact 1:1 physical visibility'
            }
            if ($Backend -eq 'dxgi' -and [bool]$proof.pointerInsideRoi)
            {
                throw 'Pointer entered the DXGI Gate ROI during the physical transfer'
            }
            $visibility.Add($proof)
            $nextProbe = $elapsed + 1000
        }
        Start-Sleep -Milliseconds 50
    }
    $receiver.Process.WaitForExit()
    if ($null -eq $publishedMilliseconds -and (Test-Path -LiteralPath $outputPath -PathType Leaf))
    {
        # The Receiver can publish and exit between two 50 ms parent polls.
        # Preserve a conservative post-publication start point rather than
        # treating a real publication as an absent timestamp.
        $publishedMilliseconds = [Int64]$watch.ElapsedMilliseconds
    }
    if (-not $resizeApplied)
    {
        throw 'Receiver finished before the required real DataWindow resize/recovery probe'
    }
    $closeText = (& $Support --close $sender.Process.Id 2>&1 | Out-String).Trim()
    $closeExit = $LASTEXITCODE
    $commands.Add([ordered]@{ Executable=$Support; Arguments=@('--close',[string]$sender.Process.Id); ExitCode=$closeExit; Output=$closeText })
    if ($closeExit -ne 0 -or -not $sender.Process.WaitForExit(15000))
    {
        throw 'Finite test-owned Sender did not complete natural WM_CLOSE shutdown'
    }
    if ($receiver.Process.ExitCode -ne 0 -or $sender.Process.ExitCode -ne 0)
    {
        throw "Physical file child failed: sender=$($sender.Process.ExitCode) receiver=$($receiver.Process.ExitCode)"
    }

    $senderFinal = Read-UniqueFinal $senderTelemetry 'phase1-sender-final'
    $receiverFinal = Read-UniqueFinal $receiverTelemetry 'phase1-receiver-final'
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf) -or -not (Test-Path -LiteralPath $outputPath -PathType Leaf))
    {
        throw 'Source or accepted final file is missing'
    }
    $sourceItem = Get-Item -LiteralPath $sourcePath
    $outputItem = Get-Item -LiteralPath $outputPath
    $sourceSha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
    $outputSha256 = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash
    $external = [pscustomobject]@{
        SourceBytes=[UInt64]$sourceItem.Length
        OutputBytes=[UInt64]$outputItem.Length
        SourceSha256=$sourceSha256
        OutputSha256=$outputSha256
        SourceOutputSha256Match=$sourceItem.Length -eq $outputItem.Length -and $sourceSha256 -ceq $outputSha256
        PartFileAbsent=-not (Test-Path -LiteralPath ($outputPath + '.part'))
        ResizeApplied=$resizeApplied
        VisibilityMaintained=$visibilityMaintained
    }
    $resources = Get-Phase1FileResourceSummary @($resourceSamples) $publishedMilliseconds $policy
    Assert-Phase1FileGateRecords $senderFinal $receiverFinal $external $resources $Profile $Backend $policy
    $result.Gate = 'PASS'
    $result['Sender'] = $senderFinal
    $result['Receiver'] = $receiverFinal
    $result['External'] = $external
    $result['Resources'] = $resources
    $result['Policy'] = $policy
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
            if ($owned -eq $sender)
            {
                & $Support --close $owned.Process.Id 2>&1 | Out-Null
            }
            if (-not $owned.Process.WaitForExit(3000))
            {
                $owned.Process.Kill()
                $owned.Process.WaitForExit()
            }
        }
    }
    foreach ($entry in @(@('sender',$sender),@('receiver',$receiver)))
    {
        if ($null -ne $entry[1])
        {
            Write-NewText (Join-Path $directory ($entry[0] + '-stdout.txt')) ($entry[1].Output.GetAwaiter().GetResult())
            Write-NewText (Join-Path $directory ($entry[0] + '-stderr.txt')) ($entry[1].Error.GetAwaiter().GetResult())
            $entry[1].Process.Dispose()
        }
    }
    Write-NewJson (Join-Path $directory 'commands.json') @($commands)
    Write-NewJson (Join-Path $directory 'visibility.json') @($visibility)
    Write-NewJson (Join-Path $directory 'resize.json') @($resizeEvidence)
    Write-NewJson (Join-Path $directory 'resource-samples.json') @($resourceSamples)
    Write-NewJson (Join-Path $directory 'phase1-file-report.json') $result
    Write-Output ($result | ConvertTo-Json -Depth 24)
}
exit $exitCode

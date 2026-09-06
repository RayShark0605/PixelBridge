param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('64MiB', '500MiB', '1GiB')]
    [string]$Tier,
    [Parameter(Mandatory = $true)]
    [string]$NewRunRoot,
    [ValidateRange(30, 7200)]
    [int]$MaximumSeconds = 3600,
    [string]$BuildRoot = (Join-Path $PSScriptRoot '..\..\build-unified-release'),
    [string]$Python = '<python>'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-NewUtf8
{
    param([string]$Path, [string]$Text)
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try
    {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally
    {
        $stream.Dispose()
    }
}

function Write-NewJson
{
    param([string]$Path, [object]$Value)
    Write-NewUtf8 -Path $Path -Text (($Value | ConvertTo-Json -Depth 12) + "`n")
}

function Get-Blake3
{
    param([string]$Path)
    $code = 'from blake3 import blake3; import sys; h=blake3(); f=open(sys.argv[1],"rb",buffering=0); [(h.update(b)) for b in iter(lambda:f.read(8*1024*1024),b"")]; f.close(); print(h.hexdigest())'
    $value = (& $Python -c $code $Path | Select-Object -Last 1).Trim()
    if ($LASTEXITCODE -ne 0 -or $value -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "BLAKE3 failed for $Path"
    }
    return $value
}

function Add-ProcessArguments
{
    param([Diagnostics.ProcessStartInfo]$StartInfo, [string[]]$Arguments)
    foreach ($argument in $Arguments)
    {
        [void]$StartInfo.ArgumentList.Add($argument)
    }
}

function Update-ProcessMemoryPeak
{
    param([Diagnostics.Process]$Process, [Collections.IDictionary]$State)
    if (-not $Process -or $Process.HasExited)
    {
        return
    }
    $Process.Refresh()
    $State.samples = [int64]$State.samples + 1
    $State.peakWorkingSetBytes = [Math]::Max([int64]$State.peakWorkingSetBytes,
        [Math]::Max([int64]$Process.WorkingSet64, [int64]$Process.PeakWorkingSet64))
    $State.peakPrivateBytes = [Math]::Max([int64]$State.peakPrivateBytes, [int64]$Process.PrivateMemorySize64)
}

function Get-ExitedProcessCode
{
    param([Diagnostics.Process]$Process)
    if (-not $Process)
    {
        return $null
    }
    try
    {
        $Process.Refresh()
        return $Process.HasExited ? [int]$Process.ExitCode : $null
    }
    catch
    {
        return $null
    }
}

if (-not ('PixelBridgeConsoleStopInjector' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class PixelBridgeConsoleStopInjector
{
    private const int StandardInputHandle = -10;
    private const short KeyEvent = 0x0001;
    private const ushort VirtualKeyQ = 0x51;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct KeyEventRecord
    {
        [MarshalAs(UnmanagedType.Bool)]
        public bool KeyDown;
        public ushort RepeatCount;
        public ushort VirtualKeyCode;
        public ushort VirtualScanCode;
        public char UnicodeChar;
        public uint ControlKeyState;
    }

    [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
    private struct InputRecord
    {
        [FieldOffset(0)]
        public short EventType;
        [FieldOffset(4)]
        public KeyEventRecord KeyEvent;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetStdHandle(int standardHandle);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetConsoleMode(IntPtr consoleHandle, out uint mode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WriteConsoleInputW(IntPtr consoleInput, InputRecord[] buffer,
        uint inputRecords, out uint writtenRecords);

    private static IntPtr GetInteractiveInputHandle()
    {
        IntPtr inputHandle = GetStdHandle(StandardInputHandle);
        uint mode;
        if (inputHandle == IntPtr.Zero || inputHandle == new IntPtr(-1) || !GetConsoleMode(inputHandle, out mode))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "G21 staircase requires an attached interactive Windows console");
        }
        return inputHandle;
    }

    public static void AssertInteractiveConsole()
    {
        GetInteractiveInputHandle();
    }

    public static void SendManualStopKey()
    {
        InputRecord record = new InputRecord();
        record.EventType = KeyEvent;
        record.KeyEvent.KeyDown = true;
        record.KeyEvent.RepeatCount = 1;
        record.KeyEvent.VirtualKeyCode = VirtualKeyQ;
        record.KeyEvent.UnicodeChar = 'q';
        uint writtenRecords;
        if (!WriteConsoleInputW(GetInteractiveInputHandle(), new InputRecord[] { record }, 1, out writtenRecords) ||
            writtenRecords != 1)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Failed to inject the bounded Encoder manual-stop key");
        }
    }
}
'@
}

[PixelBridgeConsoleStopInjector]::AssertInteractiveConsole()

$sizes = @{
    '64MiB' = 67108864L
    '500MiB' = 524288000L
    '1GiB' = 1073741824L
}
$sourceBytes = [int64]$sizes[$Tier]
$runRoot = [IO.Path]::GetFullPath($NewRunRoot)
$build = [IO.Path]::GetFullPath($BuildRoot)
$gate = Join-Path $build 'tests\UnifiedRemoteGate\Release\PBUnifiedRemoteGate.exe'
$encoder = Join-Path $build 'apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe'
if (-not [IO.Path]::IsPathFullyQualified($runRoot) -or (Test-Path -LiteralPath $runRoot) -or
    -not (Test-Path -LiteralPath $gate -PathType Leaf) -or -not (Test-Path -LiteralPath $encoder -PathType Leaf) -or
    -not (Test-Path -LiteralPath $Python -PathType Leaf))
{
    throw 'Run root must be a new absolute path and all pinned executables must exist.'
}
if (@(Get-Process -Name PixelBridgeEncoder, PixelBridgeDecoder, PBUnifiedRemoteGate -ErrorAction SilentlyContinue).Count -ne 0)
{
    throw 'A PixelBridge product or gate process is already running; refusing ambiguous ownership.'
}
[void](New-Item -ItemType Directory -Path $runRoot)

$head = (& git -C (Join-Path $PSScriptRoot '..\..') rev-parse HEAD | Select-Object -Last 1).Trim()
$gateIdentity = (& $gate --build-identity | Out-String | ConvertFrom-Json)
$encoderIdentity = (& $encoder --build-identity | Out-String | ConvertFrom-Json)
if ($head -cnotmatch '^[0-9a-f]{40}$' -or $gateIdentity.gitCommit -cne $head -or
    $encoderIdentity.gitCommit -cne $head)
{
    throw 'HEAD and embedded Gate/Encoder identities do not match; reconfigure and rebuild first.'
}

$preflightRoot = Join-Path $runRoot 'preflight'
& $gate --preflight $preflightRoot *> (Join-Path $runRoot 'preflight-process.txt')
if ($LASTEXITCODE -ne 0)
{
    throw 'Current DISPLAY1/DISPLAY2 containment preflight failed.'
}

$sourcePath = Join-Path $runRoot ("g21-local-$Tier-csprng.bin")
$random = [Security.Cryptography.RandomNumberGenerator]::Create()
$sourceWriter = [IO.File]::Open($sourcePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try
{
    $buffer = New-Object byte[] (1MB)
    [int64]$remaining = $sourceBytes
    while ($remaining -gt 0)
    {
        $count = [int][Math]::Min([int64]$buffer.Length, $remaining)
        $random.GetBytes($buffer)
        $sourceWriter.Write($buffer, 0, $count)
        $remaining -= $count
    }
    $sourceWriter.Flush($true)
}
finally
{
    $sourceWriter.Dispose()
    $random.Dispose()
}
if ((Get-Item -LiteralPath $sourcePath).Length -ne $sourceBytes)
{
    throw 'CSPRNG source length mismatch.'
}

$sourceLease = [IO.File]::Open($sourcePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$receiver = $null
$sender = $null
$receiverForcedTermination = $false
$encoderForcedTermination = $false
$receiverMemory = [ordered]@{ samples = 0L; peakWorkingSetBytes = 0L; peakPrivateBytes = 0L }
$senderMemory = [ordered]@{ samples = 0L; peakWorkingSetBytes = 0L; peakPrivateBytes = 0L }
try
{
    $sourceSha256 = (Get-FileHash -InputStream $sourceLease -Algorithm SHA256).Hash.ToLowerInvariant()
    $sourceLease.Position = 0
    $sourceBlake3 = Get-Blake3 -Path $sourcePath
    $receiverRoot = Join-Path $runRoot 'receiver'
    $receiverStdout = Join-Path $runRoot 'receiver.stdout.txt'
    $receiverStderr = Join-Path $runRoot 'receiver.stderr.txt'
    $encoderReport = Join-Path $runRoot 'encoder-report.json'
    $encoderJournal = Join-Path $runRoot 'encoder-evidence.jsonl'
    $encoderArguments = @('--headless-broadcast', '--source', $sourcePath, '--profile', 'unified', '--channel', 'local',
        '--single-monitor-fullscreen', '\\.\DISPLAY2', '--logical-fps', '15', '--seconds', [string]$MaximumSeconds,
        '--manual-stop', '--report', $encoderReport, '--journal', $encoderJournal)
    $receiverArguments = @('--receive', $receiverRoot, [string]$MaximumSeconds)
    if (($receiverArguments -join "`0").Contains($sourcePath, [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Decoder arguments unexpectedly contain the source path.'
    }
    Write-NewJson -Path (Join-Path $runRoot 'source-manifest.json') -Value ([ordered]@{
        schema = 'PixelBridge.G21.LocalStaircaseSource.1'; tier = $Tier; gitCommit = $head; bytes = $sourceBytes
        generator = 'Windows OS CSPRNG with one bounded 1 MiB buffer'; sourcePath = $sourcePath
        sha256 = $sourceSha256; blake3 = $sourceBlake3; sourceProvidedToDecoder = $false
        immutableLease = 'FileShare.Read only, held from before receiver start through both process exits'
        stopControl = 'One Q key-down record injected into the inherited interactive console after Receiver exit'
        encoderIdentity = $encoderIdentity; gateIdentity = $gateIdentity
        encoderArguments = $encoderArguments; receiverArguments = $receiverArguments
    })

    $receiver = Start-Process -FilePath $gate -ArgumentList $receiverArguments `
        -RedirectStandardOutput $receiverStdout -RedirectStandardError $receiverStderr -WindowStyle Hidden -PassThru
    Update-ProcessMemoryPeak -Process $receiver -State $receiverMemory
    Start-Sleep -Seconds 2
    if ($receiver.HasExited)
    {
        throw "Receiver exited before Encoder startup with code $($receiver.ExitCode)."
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $encoder
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardInput = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $false
    Add-ProcessArguments -StartInfo $startInfo -Arguments $encoderArguments
    $sender = [Diagnostics.Process]::new()
    $sender.StartInfo = $startInfo
    if (-not $sender.Start())
    {
        throw 'Encoder process did not start.'
    }
    Update-ProcessMemoryPeak -Process $receiver -State $receiverMemory
    Update-ProcessMemoryPeak -Process $sender -State $senderMemory

    $deadline = [DateTime]::UtcNow.AddSeconds($MaximumSeconds + 45)
    while (-not $receiver.HasExited -and [DateTime]::UtcNow -lt $deadline)
    {
        if ($sender.HasExited)
        {
            throw "Encoder exited before Receiver with code $($sender.ExitCode)."
        }
        Update-ProcessMemoryPeak -Process $receiver -State $receiverMemory
        Update-ProcessMemoryPeak -Process $sender -State $senderMemory
        Start-Sleep -Seconds 1
        $receiver.Refresh()
    }
    if (-not $receiver.HasExited)
    {
        Stop-Process -Id $receiver.Id -Force -ErrorAction SilentlyContinue
        [void]$receiver.WaitForExit(5000)
        $receiverForcedTermination = $true
        throw 'Receiver supervisor deadline exceeded.'
    }
    if (-not $sender.HasExited)
    {
        Update-ProcessMemoryPeak -Process $sender -State $senderMemory
        [PixelBridgeConsoleStopInjector]::SendManualStopKey()
    }
    if (-not $sender.WaitForExit(30000))
    {
        Stop-Process -Id $sender.Id -Force -ErrorAction SilentlyContinue
        [void]$sender.WaitForExit(5000)
        $encoderForcedTermination = $true
        throw 'Encoder did not stop within 30 seconds of the bounded supervisor signal.'
    }
    $encoderStdout = $sender.StandardOutput.ReadToEnd()
    $encoderStderr = $sender.StandardError.ReadToEnd()
    Write-NewUtf8 -Path (Join-Path $runRoot 'encoder.stdout.txt') -Text $encoderStdout
    Write-NewUtf8 -Path (Join-Path $runRoot 'encoder.stderr.txt') -Text $encoderStderr
    if ($receiver.ExitCode -ne 0 -or $sender.ExitCode -ne 0)
    {
        throw "Staircase process failure: receiver=$($receiver.ExitCode), encoder=$($sender.ExitCode)."
    }

    $finalPath = Join-Path $receiverRoot 'final.json'
    $gatePath = Join-Path $receiverRoot 'receiver-checks.json'
    $final = Get-Content -LiteralPath $finalPath -Raw | ConvertFrom-Json
    $checks = Get-Content -LiteralPath $gatePath -Raw | ConvertFrom-Json
    $encoderFinal = Get-Content -LiteralPath $encoderReport -Raw | ConvertFrom-Json
    $laneFailures = @($final.unifiedTelemetry.lanes | Where-Object {
        [int64]$_.fec.crcFailures -ne 0 -or [int64]$_.fec.identityFailures -ne 0
    })
    $residualStateFiles = @(Get-ChildItem -LiteralPath $receiverRoot -Recurse -File | Where-Object {
        $_.Name -match '\.(part|resume)(\.|$)'
    })
    if ($final.state -cne 'Completed' -or -not $final.publish.wholeDigestVerified -or
        -not $final.publish.renameSucceeded -or -not $final.publish.finalReopenVerified -or -not $final.publish.published -or
        -not $checks.receiverLocalChecksPassed -or -not $checks.hard16KiBFrameMetricPassed -or
        [int64]$final.fileBytes -ne $sourceBytes -or [int64]$final.recovery.verifiedRawBytes -ne $sourceBytes -or
        [int64]$final.recovery.verifiedSegments -le 0 -or
        [int64]$final.recovery.verifiedSegments -ne [int64]$encoderFinal.scheduler.segmentCount -or
        -not $final.unifiedTelemetry.frameCoverageComplete -or $final.unifiedTelemetry.counterOverflow -or
        $laneFailures.Count -ne 0 -or $residualStateFiles.Count -ne 0 -or
        $final.remoteGate.sourceOrOracleProvided -or $final.capture.actualBackend -notin @('WGC', 'DXGI') -or
        $encoderFinal.state -cne 'Stopped' -or -not $encoderFinal.preparation.sourceStabilityVerified -or
        [int64]$encoderFinal.fileBytes -ne $sourceBytes -or [int64]$encoderFinal.configuredLogicalFps -ne 15 -or
        [int64]$receiverMemory.samples -le 0 -or [int64]$senderMemory.samples -le 0 -or
        [int64]$receiverMemory.peakWorkingSetBytes -le 0 -or [int64]$senderMemory.peakWorkingSetBytes -le 0)
    {
        throw 'Run artifacts do not satisfy publication, integrity, cleanup, lane, memory and 16 KiB gates.'
    }
    $recoveredPath = [string]$final.publish.finalPath
    $recoveredSha256 = (Get-FileHash -LiteralPath $recoveredPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $recoveredBlake3 = Get-Blake3 -Path $recoveredPath
    $digestPass = $sourceSha256 -ceq $recoveredSha256 -and $sourceBlake3 -ceq $recoveredBlake3
    if (-not $digestPass)
    {
        throw 'Independent SHA-256/BLAKE3 source-to-published verification failed.'
    }
    if ($sourceBlake3 -cne [string]$encoderFinal.sourceWholeFileDigest -or
        $sourceBlake3 -cne [string]$final.publish.wholeFileDigest)
    {
        throw 'Independent BLAKE3 does not match the sender and receiver whole-file digests.'
    }

    Write-NewJson -Path (Join-Path $runRoot 'external-digest-audit.json') -Value ([ordered]@{
        schema = 'PixelBridge.G21.LocalStaircaseExternalDigestAudit.1'; tier = $Tier; bytes = $sourceBytes
        source = @{ sha256 = $sourceSha256; blake3 = $sourceBlake3 }
        published = @{ path = $recoveredPath; sha256 = $recoveredSha256; blake3 = $recoveredBlake3 }
        sha256Equal = $sourceSha256 -ceq $recoveredSha256; blake3Equal = $sourceBlake3 -ceq $recoveredBlake3
        authority = 'Independent post-run reads; source was never passed to Decoder'
    })
    Write-NewJson -Path (Join-Path $runRoot 'process-exits.json') -Value ([ordered]@{
        receiverExit = $receiver.ExitCode; encoderExit = $sender.ExitCode; supervisorForcedTermination = $false
        memorySampleIntervalMilliseconds = 1000; receiverMemory = $receiverMemory; encoderMemory = $senderMemory
    })
    Write-NewJson -Path (Join-Path $runRoot 'staircase-verification.json') -Value ([ordered]@{
        schema = 'PixelBridge.G21.LocalStaircaseVerification.1'; tier = $Tier; bytes = $sourceBytes; gitCommit = $head
        allSegmentsCompleted = $true; verifiedSegments = [int64]$final.recovery.verifiedSegments
        segmentCount = [int64]$encoderFinal.scheduler.segmentCount; wholeDigestVerified = $true
        safePublish = $true; finalReopenVerified = $true; residualPartOrResumeFiles = @()
        laneCrcAndIdentityFailuresZero = $true; receiverLocalChecksPassed = $true
        hard16KiBFrameMetricPassed = $true; engineering32KiBTargetReached = [bool]$checks.engineering32KiBTargetReached
        senderConfiguredLogicalFps = [int64]$encoderFinal.configuredLogicalFps
        senderSubmittedLogicalFps = [double]$encoderFinal.observedSubmittedLogicalFps
        receiverUniqueVisualFps = [double]$final.unifiedTelemetry.uniqueVisualFps
        receiverUniqueLogicalFrames = [int64]$final.unifiedTelemetry.uniqueLogicalFrames
        receiverElapsedMilliseconds = [int64]$final.runEndedUnixMilliseconds - [int64]$final.runStartedUnixMilliseconds
        verifiedEncodedBytesPerUniqueFrame = [double]$final.verifiedEncodedBytesPerUniqueFrame
        receiverOuterResourceRejections = [int64]$final.remoteGate.outerResourceRejections
        receiverOuterConflictRejections = [int64]$final.remoteGate.outerConflictRejections
        receiverOuterDeferredResourceBusyCount = [int64]$final.remoteGate.outerDeferredResourceBusyCount
        receiverOuterFecQuotaExceededCount = [int64]$final.remoteGate.outerFecQuotaExceededCount
        receiverOuterPeakActiveDecoderCount = [int64]$final.remoteGate.outerPeakActiveDecoderCount
        receiverOuterActiveDecoderLimit = [int64]$final.remoteGate.outerActiveDecoderLimit
        receiverOuterPeakReservedDecoderBytes = [int64]$final.remoteGate.outerPeakReservedDecoderBytes
        receiverOuterTotalDecoderByteLimit = [int64]$final.remoteGate.outerTotalDecoderByteLimit
        receiverOuterPeakOrphanCachedBytes = [int64]$final.remoteGate.outerPeakOrphanCachedBytes
        receiverProcessMemory = $receiverMemory; encoderProcessMemory = $senderMemory
        memoryBoundBasis = 'Sampled OS process peaks plus protocol-owned active-window peaks; cross-tier comparison is required for O(active Segment window) conclusion'
        sourceOrOracleProvidedToDecoder = $false; independentSha256AndBlake3Equal = $true
    })
    Write-Output ("PASS: {0}; run={1}; uniqueFrames={2}; uniqueFps={3}; bytesPerUnique={4}" -f
        $Tier, $runRoot, $final.unifiedTelemetry.uniqueLogicalFrames,
        $final.unifiedTelemetry.uniqueVisualFps, $final.verifiedEncodedBytesPerUniqueFrame)
}
catch
{
    $failure = $_
    if ($receiver -and -not $receiver.HasExited)
    {
        Stop-Process -Id $receiver.Id -Force -ErrorAction SilentlyContinue
        [void]$receiver.WaitForExit(5000)
        $receiverForcedTermination = $true
    }
    if ($sender -and -not $sender.HasExited)
    {
        Stop-Process -Id $sender.Id -Force -ErrorAction SilentlyContinue
        [void]$sender.WaitForExit(5000)
        $encoderForcedTermination = $true
    }
    $failureProcessPath = Join-Path $runRoot 'failure-process-state.json'
    if (-not (Test-Path -LiteralPath $failureProcessPath))
    {
        try
        {
            Write-NewJson -Path $failureProcessPath -Value ([ordered]@{
                schema = 'PixelBridge.G21.LocalStaircaseFailureProcessState.1'
                failure = $failure.Exception.Message
                receiverExit = Get-ExitedProcessCode -Process $receiver
                encoderExit = Get-ExitedProcessCode -Process $sender
                receiverForcedTermination = $receiverForcedTermination
                encoderForcedTermination = $encoderForcedTermination
                memorySampleIntervalMilliseconds = 1000
                receiverMemory = $receiverMemory; encoderMemory = $senderMemory
            })
        }
        catch
        {
        }
    }
    $failurePath = Join-Path $runRoot 'supervisor-failure.txt'
    if (-not (Test-Path -LiteralPath $failurePath))
    {
        Write-NewUtf8 -Path $failurePath -Text ($failure.Exception.ToString() + "`n")
    }
    throw $failure
}
finally
{
    $sourceLease.Dispose()
    if ($receiver) { $receiver.Dispose() }
    if ($sender) { $sender.Dispose() }
}

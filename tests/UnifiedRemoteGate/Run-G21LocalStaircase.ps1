param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('64MiB', '500MiB', '1GiB')]
    [string]$Tier,
    [Parameter(Mandatory = $true)]
    [string]$NewRunRoot,
    [ValidateRange(30, 3600)]
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
    Write-NewJson -Path (Join-Path $runRoot 'source-manifest.json') -Value ([ordered]@{
        schema = 'PixelBridge.G21.LocalStaircaseSource.1'; tier = $Tier; gitCommit = $head; bytes = $sourceBytes
        generator = 'Windows OS CSPRNG with one bounded 1 MiB buffer'; sourcePath = $sourcePath
        sha256 = $sourceSha256; blake3 = $sourceBlake3; sourceProvidedToDecoder = $false
        immutableLease = 'FileShare.Read only, held from before receiver start through both process exits'
        stopControl = 'One Q key-down record injected into the inherited interactive console after Receiver exit'
        encoderIdentity = $encoderIdentity; gateIdentity = $gateIdentity; encoderArguments = $encoderArguments
    })

    $receiver = Start-Process -FilePath $gate -ArgumentList @('--receive', $receiverRoot, [string]$MaximumSeconds) `
        -RedirectStandardOutput $receiverStdout -RedirectStandardError $receiverStderr -WindowStyle Hidden -PassThru
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

    $deadline = [DateTime]::UtcNow.AddSeconds($MaximumSeconds + 45)
    while (-not $receiver.HasExited -and [DateTime]::UtcNow -lt $deadline)
    {
        if ($sender.HasExited)
        {
            throw "Encoder exited before Receiver with code $($sender.ExitCode)."
        }
        Start-Sleep -Seconds 1
        $receiver.Refresh()
    }
    if (-not $receiver.HasExited)
    {
        Stop-Process -Id $receiver.Id -Force
        throw 'Receiver supervisor deadline exceeded.'
    }
    if (-not $sender.HasExited)
    {
        [PixelBridgeConsoleStopInjector]::SendManualStopKey()
    }
    if (-not $sender.WaitForExit(30000))
    {
        Stop-Process -Id $sender.Id -Force
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
    if ($final.state -cne 'Completed' -or -not $final.publish.wholeDigestVerified -or
        -not $final.publish.renameSucceeded -or -not $final.publish.finalReopenVerified -or
        -not $checks.receiverLocalChecksPassed -or -not $checks.hard16KiBFrameMetricPassed -or
        [int64]$final.fileBytes -ne $sourceBytes)
    {
        throw 'Receiver artifacts do not satisfy publication and 16 KiB gates.'
    }
    $recoveredPath = [string]$final.publish.finalPath
    $recoveredSha256 = (Get-FileHash -LiteralPath $recoveredPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $recoveredBlake3 = Get-Blake3 -Path $recoveredPath
    $digestPass = $sourceSha256 -ceq $recoveredSha256 -and $sourceBlake3 -ceq $recoveredBlake3
    if (-not $digestPass)
    {
        throw 'Independent SHA-256/BLAKE3 source-to-published verification failed.'
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
    })
    Write-Output ("PASS: {0}; run={1}; uniqueFrames={2}; uniqueFps={3}; bytesPerUnique={4}" -f
        $Tier, $runRoot, $final.unifiedTelemetry.uniqueLogicalFrames,
        $final.unifiedTelemetry.uniqueVisualFps, $final.verifiedEncodedBytesPerUniqueFrame)
}
catch
{
    if ($receiver -and -not $receiver.HasExited)
    {
        Stop-Process -Id $receiver.Id -Force -ErrorAction SilentlyContinue
    }
    if ($sender -and -not $sender.HasExited)
    {
        Stop-Process -Id $sender.Id -Force -ErrorAction SilentlyContinue
    }
    $failurePath = Join-Path $runRoot 'supervisor-failure.txt'
    if (-not (Test-Path -LiteralPath $failurePath))
    {
        Write-NewUtf8 -Path $failurePath -Text ($_.Exception.ToString() + "`n")
    }
    throw
}
finally
{
    $sourceLease.Dispose()
    if ($receiver) { $receiver.Dispose() }
    if ($sender) { $sender.Dispose() }
}

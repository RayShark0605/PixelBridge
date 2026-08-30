[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$MonitorDeviceName,
    [Parameter(Mandatory = $true)]
    [int]$MonitorLeft,
    [Parameter(Mandatory = $true)]
    [int]$MonitorTop,
    [Parameter(Mandatory = $true)]
    [int]$MonitorRight,
    [Parameter(Mandatory = $true)]
    [int]$MonitorBottom,
    [Parameter(Mandatory = $true)]
    [int]$MonitorOriginX,
    [Parameter(Mandatory = $true)]
    [int]$MonitorOriginY,
    [ValidateSet('wgc', 'dxgi')]
    [string]$Backend = 'wgc',
    [ValidateSet('direct')]
    [string]$Profile = 'direct',
    [ValidateSet('off', 'on')]
    [string]$Compression = 'off',
    [ValidateRange(10, 300)]
    [int]$DecoderTimeoutSeconds = 60,
    [string]$PythonExecutable = 'D:\Python3.12.9\python.exe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-FromSourceRoot([string]$path, [string]$sourceRoot)
{
    if ([IO.Path]::IsPathRooted($path))
    {
        return [IO.Path]::GetFullPath($path)
    }
    return [IO.Path]::GetFullPath((Join-Path $sourceRoot $path))
}

function Wait-Until([scriptblock]$condition, [int]$timeoutMilliseconds, [string]$description)
{
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMilliseconds)
    do
    {
        $value = & $condition
        if ($null -ne $value -and $false -ne $value)
        {
            return $value
        }
        Start-Sleep -Milliseconds 100
    }
    while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $description"
}

function Get-ElementsByControlType($root, $controlType)
{
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $controlType)
    return $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Find-NamedElement($root, $controlType, [string]$name)
{
    $elements = @(Get-ElementsByControlType $root $controlType)
    for ($index = 0; $index -lt $elements.Count; $index++)
    {
        $element = $elements.Item($index)
        if ($element.Current.Name -ceq $name)
        {
            return $element
        }
    }
    return $null
}

function Find-TextContaining($root, [string]$text)
{
    $elements = @(Get-ElementsByControlType $root ([System.Windows.Automation.ControlType]::Text))
    for ($index = 0; $index -lt $elements.Count; $index++)
    {
        $element = $elements.Item($index)
        if ($element.Current.Name.Contains($text, [StringComparison]::Ordinal))
        {
            return $element
        }
    }
    return $null
}

function Find-ComboValueStartingWith($root, [string]$itemPrefix)
{
    $combos = @(Get-ElementsByControlType $root ([System.Windows.Automation.ControlType]::ComboBox))
    for ($comboIndex = 0; $comboIndex -lt $combos.Count; $comboIndex++)
    {
        $combo = $combos.Item($comboIndex)
        $valueObject = $null
        if (-not $combo.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern,
            [ref]$valueObject))
        {
            continue
        }
        $value = ([System.Windows.Automation.ValuePattern]$valueObject).Current.Value
        if ($value.StartsWith($itemPrefix, [StringComparison]::Ordinal))
        {
            return $value
        }
    }
    return $null
}

function Invoke-Button($root, [string]$name)
{
    $button = Find-NamedElement $root ([System.Windows.Automation.ControlType]::Button) $name
    if ($null -eq $button -or -not $button.Current.IsEnabled)
    {
        throw "Button is missing or disabled: $name"
    }
    $invokeObject = $null
    if (-not $button.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invokeObject))
    {
        throw "Button does not expose InvokePattern: $name"
    }
    ([System.Windows.Automation.InvokePattern]$invokeObject).Invoke()
}

function Get-CheckboxState($root, [string]$name)
{
    $checkbox = Find-NamedElement $root ([System.Windows.Automation.ControlType]::CheckBox) $name
    if ($null -eq $checkbox -or -not $checkbox.Current.IsEnabled)
    {
        throw "Checkbox is missing or disabled: $name"
    }
    $toggleObject = $null
    if (-not $checkbox.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern,
        [ref]$toggleObject))
    {
        throw "Checkbox does not expose TogglePattern: $name"
    }
    return ([System.Windows.Automation.TogglePattern]$toggleObject).Current.ToggleState -eq
        [System.Windows.Automation.ToggleState]::On
}

function Get-ProcessWindows([int]$processId)
{
    $records = [Collections.Generic.List[object]]::new()
    foreach ($window in [PixelBridgeGuiSmokeNative]::ForProcess($processId))
    {
        $windowRect = [PixelBridgeGuiSmokeNative+RECT]::new()
        $clientRect = [PixelBridgeGuiSmokeNative+RECT]::new()
        [void][PixelBridgeGuiSmokeNative]::GetWindowRect($window, [ref]$windowRect)
        [void][PixelBridgeGuiSmokeNative]::GetClientScreenRect($window, [ref]$clientRect)
        $records.Add([ordered]@{
            handle = ('0x{0:X}' -f $window.ToInt64())
            title = [PixelBridgeGuiSmokeNative]::Text($window)
            className = [PixelBridgeGuiSmokeNative]::ClassName($window)
            visible = [PixelBridgeGuiSmokeNative]::IsWindowVisible($window)
            windowRect = @($windowRect.Left, $windowRect.Top, $windowRect.Right, $windowRect.Bottom)
            clientRect = @($clientRect.Left, $clientRect.Top, $clientRect.Right, $clientRect.Bottom)
        })
    }
    return @($records)
}

$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildRootFull = Resolve-FromSourceRoot $BuildRoot $sourceRoot
$evidenceRootFull = Resolve-FromSourceRoot $EvidenceRoot $sourceRoot
$sourceFull = Resolve-FromSourceRoot $Source $sourceRoot
$pythonFull = Resolve-FromSourceRoot $PythonExecutable $sourceRoot
$encoder = Join-Path $buildRootFull 'apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe'
$decoder = Join-Path $buildRootFull 'apps\PixelBridgeDecoder\Release\PixelBridgeDecoder.exe'
foreach ($requiredFile in @($encoder, $decoder, $sourceFull, $pythonFull))
{
    if (-not [IO.File]::Exists($requiredFile))
    {
        throw "Required file is missing: $requiredFile"
    }
}
if ([IO.Directory]::Exists($evidenceRootFull) -or [IO.File]::Exists($evidenceRootFull))
{
    throw "EvidenceRoot already exists; output is create-only: $evidenceRootFull"
}
[IO.Directory]::CreateDirectory($evidenceRootFull) | Out-Null
$outputRoot = Join-Path $evidenceRootFull 'output'
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$decoderReport = Join-Path $evidenceRootFull 'decoder-report.json'
$decoderStdout = Join-Path $evidenceRootFull 'decoder.stdout.log'
$decoderStderr = Join-Path $evidenceRootFull 'decoder.stderr.log'
$summaryPath = Join-Path $evidenceRootFull 'gui-integration-summary.json'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class PixelBridgeGuiSmokeNative
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref POINT point);
    [DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr window, ref WINDOWPLACEMENT placement);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct WINDOWPLACEMENT
    {
        public int Length, Flags, ShowCommand;
        public POINT MinimumPosition, MaximumPosition;
        public RECT NormalPosition;
    }
    public static IntPtr[] ForProcess(int requestedProcessId)
    {
        var result = new List<IntPtr>();
        EnumWindows((window, parameter) =>
        {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == requestedProcessId)
            {
                result.Add(window);
            }
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static IntPtr FindWindow(int requestedProcessId, string requestedTitle)
    {
        foreach (IntPtr window in ForProcess(requestedProcessId))
        {
            if (Text(window) == requestedTitle)
            {
                return window;
            }
        }
        return IntPtr.Zero;
    }
    public static string Text(IntPtr window)
    {
        var value = new StringBuilder(512);
        GetWindowText(window, value, value.Capacity);
        return value.ToString();
    }
    public static string ClassName(IntPtr window)
    {
        var value = new StringBuilder(256);
        GetClassName(window, value, value.Capacity);
        return value.ToString();
    }
    public static bool GetClientScreenRect(IntPtr window, out RECT rect)
    {
        RECT local;
        if (!GetClientRect(window, out local))
        {
            rect = new RECT();
            return false;
        }
        POINT topLeft = new POINT { X = local.Left, Y = local.Top };
        POINT bottomRight = new POINT { X = local.Right, Y = local.Bottom };
        if (!ClientToScreen(window, ref topLeft) || !ClientToScreen(window, ref bottomRight))
        {
            rect = new RECT();
            return false;
        }
        rect = new RECT { Left = topLeft.X, Top = topLeft.Y, Right = bottomRight.X, Bottom = bottomRight.Y };
        return true;
    }
}
'@

$registryPath = 'HKCU:\Software\PixelBridge\PixelBridgeEncoder\ui'
$registrySubKey = 'Software\PixelBridge\PixelBridgeEncoder\ui'
$registryExisted = Test-Path -LiteralPath $registryPath
if (-not $registryExisted)
{
    New-Item -Path $registryPath -Force | Out-Null
}
$settingsToPreserve = @('windowGeometry', 'lastInputPath', 'compressionEnabled', 'advancedExpanded',
    'lastMonitorDevice')
$settingsBackup = [ordered]@{}
$registryKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($registrySubKey, $true)
if ($null -eq $registryKey)
{
    throw "Unable to open writable QSettings registry key: $registryPath"
}
$existingNames = @($registryKey.GetValueNames())
foreach ($settingName in $settingsToPreserve)
{
    if ($existingNames -ccontains $settingName)
    {
        $settingsBackup[$settingName] = [ordered]@{
            exists = $true
            value = $registryKey.GetValue($settingName, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            kind = $registryKey.GetValueKind($settingName)
        }
    }
    else
    {
        $settingsBackup[$settingName] = [ordered]@{ exists = $false }
    }
}
$registryKey.SetValue('lastInputPath', $sourceFull, [Microsoft.Win32.RegistryValueKind]::String)
$registryKey.SetValue('compressionEnabled', $(if ($Compression -ceq 'on') { 'true' } else { 'false' }),
    [Microsoft.Win32.RegistryValueKind]::String)
$registryKey.SetValue('lastMonitorDevice', $MonitorDeviceName, [Microsoft.Win32.RegistryValueKind]::String)
$registryKey.Close()

$encoderProcess = $null
$decoderProcess = $null
$qtWindow = [IntPtr]::Zero
$root = $null
$evidence = [ordered]@{
    schema = 'PixelBridge.EncoderGuiLocalDesktopSmoke.1'
    status = 'FAIL'
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    source = $sourceFull
    buildRoot = $buildRootFull
    evidenceRoot = $evidenceRootFull
    requestedMonitor = [ordered]@{
        deviceName = $MonitorDeviceName
        bounds = @($MonitorLeft, $MonitorTop, $MonitorRight, $MonitorBottom)
        dataWindowOrigin = @($MonitorOriginX, $MonitorOriginY)
    }
    backend = $Backend
    profile = $Profile
    compression = $Compression
    uiAutomation = [ordered]@{}
    decoder = [ordered]@{}
    digestVerification = [ordered]@{}
    settingsRestored = $false
    errors = [Collections.Generic.List[string]]::new()
}
$failure = $null
try
{
    $encoderProcess = Start-Process -FilePath $encoder -ArgumentList @('--gui-integration-smoke', $MonitorDeviceName) `
        -WorkingDirectory (Split-Path -Parent $encoder) -WindowStyle Minimized -PassThru
    $qtWindow = Wait-Until {
        $candidate = [PixelBridgeGuiSmokeNative]::FindWindow($encoderProcess.Id, 'PixelBridge Encoder')
        if ($candidate -eq [IntPtr]::Zero) { return $null }
        return $candidate
    } 10000 'the formal Encoder Qt window'

    $placement = [PixelBridgeGuiSmokeNative+WINDOWPLACEMENT]::new()
    $placement.Length = [Runtime.InteropServices.Marshal]::SizeOf([type][PixelBridgeGuiSmokeNative+WINDOWPLACEMENT])
    if (-not [PixelBridgeGuiSmokeNative]::GetWindowPlacement($qtWindow, [ref]$placement))
    {
        throw 'GetWindowPlacement failed for Encoder GUI'
    }
    $normal = $placement.NormalPosition
    if ($normal.Left -lt $MonitorLeft -or $normal.Top -lt $MonitorTop -or
        $normal.Right -gt $MonitorRight -or $normal.Bottom -gt $MonitorBottom)
    {
        throw "Encoder GUI normal position is not wholly inside the requested monitor: [$($normal.Left),$($normal.Top),$($normal.Right),$($normal.Bottom)]"
    }
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($qtWindow)
    $sourceValidation = Wait-Until {
        $candidate = Find-TextContaining $root ([IO.Path]::GetFileName($sourceFull))
        if ($null -eq $candidate) { return $null }
        return $candidate.Current.Name
    } 5000 'GUI source file validation'
    $compressionChecked = Get-CheckboxState $root '启用 Segment 压缩'
    if ($compressionChecked -ne ($Compression -ceq 'on'))
    {
        throw 'Encoder GUI did not restore the requested compression preference'
    }
    $profilePrefix = if ($Profile -ceq 'direct') { 'Direct-Level 2x2' } else { 'Shape+Chroma' }
    $selectedProfile = Wait-Until { Find-ComboValueStartingWith $root $profilePrefix } 5000 'Visual Profile binding'
    $selectedMonitor = Wait-Until { Find-ComboValueStartingWith $root "$MonitorDeviceName ·" } 5000 `
        'persisted target monitor binding'
    $monitorDetails = Wait-Until {
        $candidate = Find-TextContaining $root "Physical [$MonitorLeft,$MonitorTop]"
        if ($null -eq $candidate) { return $null }
        if (-not $candidate.Current.Name.Contains("Data Window origin [$MonitorOriginX,$MonitorOriginY]",
            [StringComparison]::Ordinal)) { return $null }
        return $candidate.Current.Name
    } 5000 'right-monitor Data Window origin binding'

    [void][PixelBridgeGuiSmokeNative]::ShowWindow($qtWindow, 4)
    Start-Sleep -Milliseconds 250
    $guiWindowRect = [PixelBridgeGuiSmokeNative+RECT]::new()
    if (-not [PixelBridgeGuiSmokeNative]::GetWindowRect($qtWindow, [ref]$guiWindowRect) -or
        $guiWindowRect.Left -lt $MonitorLeft -or $guiWindowRect.Top -lt $MonitorTop -or
        $guiWindowRect.Right -gt $MonitorRight -or $guiWindowRect.Bottom -gt $MonitorBottom)
    {
        throw "Visible Encoder GUI is not wholly inside the requested monitor: [$($guiWindowRect.Left),$($guiWindowRect.Top),$($guiWindowRect.Right),$($guiWindowRect.Bottom)]"
    }
    Invoke-Button $root '开始广播'
    $broadcastingState = Wait-Until {
        $candidate = Find-NamedElement $root ([System.Windows.Automation.ControlType]::Text) 'Broadcasting'
        if ($null -eq $candidate) { return $null }
        return $candidate.Current.Name
    } 20000 'Encoder GUI Broadcasting state'

    $roiRight = $MonitorOriginX + 1920
    $roiBottom = $MonitorOriginY + 1080
    $dataWindowRecord = Wait-Until {
        foreach ($record in Get-ProcessWindows $encoderProcess.Id)
        {
            if ($record.visible -and $record.clientRect[0] -eq $MonitorOriginX -and
                $record.clientRect[1] -eq $MonitorOriginY -and $record.clientRect[2] -eq $roiRight -and
                $record.clientRect[3] -eq $roiBottom)
            {
                return $record
            }
        }
        return $null
    } 10000 'the exact right-screen D3D11 Data Window client rectangle'

    $decoderArguments = '--headless-receive --output-dir "{0}" --backend {1} --profile {2} --roi {3} {4} {5} {6} --timeout {7} --report "{8}"' -f
        $outputRoot,$Backend,$Profile,$MonitorOriginX,$MonitorOriginY,$roiRight,$roiBottom,$DecoderTimeoutSeconds,$decoderReport
    $decoderProcess = Start-Process -FilePath $decoder -ArgumentList $decoderArguments `
        -WorkingDirectory (Split-Path -Parent $decoder) -WindowStyle Hidden `
        -RedirectStandardOutput $decoderStdout -RedirectStandardError $decoderStderr -PassThru
    if (-not $decoderProcess.WaitForExit(($DecoderTimeoutSeconds + 15) * 1000))
    {
        throw 'Decoder did not exit within its bounded timeout'
    }
    if ($decoderProcess.ExitCode -ne 0)
    {
        throw "Decoder failed with exit code $($decoderProcess.ExitCode)"
    }
    $encoderProcess.Refresh()
    if ($encoderProcess.HasExited)
    {
        throw 'Encoder GUI exited before Decoder completed'
    }
    $stateAtDecoderCompletion = Wait-Until {
        $candidate = Find-NamedElement $root ([System.Windows.Automation.ControlType]::Text) 'Broadcasting'
        if ($null -eq $candidate) { return $null }
        return $candidate.Current.Name
    } 3000 'Encoder GUI to remain Broadcasting after Decoder completion'
    $cycleText = (Find-TextContaining $root '不是文件恢复进度').Current.Name
    $telemetryText = (Find-TextContaining $root 'PresentationEpoch=').Current.Name
    $sessionText = (Find-TextContaining $root 'SessionTag ').Current.Name

    Start-Sleep -Milliseconds 500
    Invoke-Button $root '停止广播'
    $stoppedState = Wait-Until {
        $candidate = Find-NamedElement $root ([System.Windows.Automation.ControlType]::Text) 'Stopped'
        if ($null -eq $candidate) { return $null }
        return $candidate.Current.Name
    } 20000 'Encoder GUI Stopped state after UI Stop'

    $decoderJson = Get-Content -LiteralPath $decoderReport -Raw | ConvertFrom-Json
    $outputFiles = @(Get-ChildItem -LiteralPath $outputRoot -File)
    $partFiles = @(Get-ChildItem -LiteralPath $evidenceRootFull -Recurse -File -Filter '*.part')
    if ($outputFiles.Count -ne 1 -or $partFiles.Count -ne 0)
    {
        throw "Expected exactly one final output and no .part files; final=$($outputFiles.Count), part=$($partFiles.Count)"
    }
    $output = $outputFiles[0].FullName
    $sourceSha256 = (Get-FileHash -LiteralPath $sourceFull -Algorithm SHA256).Hash.ToLowerInvariant()
    $outputSha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
    $pythonCode = 'import json,sys; from blake3 import blake3; print(json.dumps([blake3(open(p,"rb").read()).hexdigest() for p in sys.argv[1:]]))'
    $blakeValues = (& $pythonFull -c $pythonCode $sourceFull $output | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $blakeValues.Count -ne 2)
    {
        throw 'Independent BLAKE3 calculation failed'
    }
    $sourceBytes = (Get-Item -LiteralPath $sourceFull).Length
    $outputBytes = (Get-Item -LiteralPath $output).Length
    $telemetryDigestMatch = [regex]::Match($telemetryText, 'Digest=([0-9a-f]{64})')
    $digestPass = $sourceBytes -eq $outputBytes -and $sourceSha256 -ceq $outputSha256 -and
        [string]$blakeValues[0] -ceq [string]$blakeValues[1] -and
        [string]$blakeValues[0] -ceq [string]$decoderJson.wholeFileDigest -and
        $telemetryDigestMatch.Success -and $telemetryDigestMatch.Groups[1].Value -ceq [string]$blakeValues[0]
    if (-not $digestPass -or [string]$decoderJson.state -cne 'Completed' -or
        -not [bool]$decoderJson.wholeFileDigestVerified -or -not [bool]$decoderJson.finalPublishSucceeded)
    {
        throw 'GUI-driven LocalDesktop output or completion verification failed'
    }

    $evidence.uiAutomation = [ordered]@{
        formalExecutable = $encoder
        sourceValidation = $sourceValidation
        selectedProfile = $selectedProfile
        selectedMonitor = $selectedMonitor
        monitorDetails = $monitorDetails
        guiWindowRect = @($guiWindowRect.Left, $guiWindowRect.Top, $guiWindowRect.Right, $guiWindowRect.Bottom)
        dataWindow = $dataWindowRecord
        stateAfterStart = $broadcastingState
        stateAtDecoderCompletion = $stateAtDecoderCompletion
        encoderAliveAtDecoderCompletion = $true
        cycleTextAtDecoderCompletion = $cycleText
        telemetryAtDecoderCompletion = $telemetryText
        sessionAtDecoderCompletion = $sessionText
        stateAfterUiStop = $stoppedState
        mouseInputUsed = $false
        keyboardInputUsed = $false
    }
    $evidence.decoder = [ordered]@{
        formalExecutable = $decoder
        report = $decoderReport
        state = $decoderJson.state
        actualBackend = $decoderJson.actualBackend
        descriptorKnown = $decoderJson.descriptorKnown
        wholeFileDigestVerified = $decoderJson.wholeFileDigestVerified
        finalPublishSucceeded = $decoderJson.finalPublishSucceeded
        outputPath = $decoderJson.outputPath
        verifiedEncodedGoodputBitsPerSecond = $decoderJson.verifiedEncodedGoodputBitsPerSecond
        recoveryRuntimeMilliseconds = $decoderJson.recoveryRuntimeMilliseconds
    }
    $evidence.digestVerification = [ordered]@{
        output = $output
        sourceBytes = $sourceBytes
        outputBytes = $outputBytes
        sourceSha256 = $sourceSha256
        outputSha256 = $outputSha256
        sourceBlake3 = [string]$blakeValues[0]
        outputBlake3 = [string]$blakeValues[1]
        decoderWholeFileDigest = [string]$decoderJson.wholeFileDigest
        encoderGuiTelemetryDigest = $telemetryDigestMatch.Groups[1].Value
        noPartFilesRemain = $true
        pass = $true
    }
    $evidence.status = 'PASS'
}
catch
{
    $failure = $_
    $evidence.errors.Add($_.Exception.ToString() + [Environment]::NewLine +
        $_.InvocationInfo.PositionMessage + [Environment]::NewLine + $_.ScriptStackTrace)
}
finally
{
    if ($null -ne $decoderProcess -and -not $decoderProcess.HasExited)
    {
        $decoderProcess.Kill($true)
        $decoderProcess.WaitForExit()
    }
    if ($null -ne $encoderProcess -and -not $encoderProcess.HasExited)
    {
        if ($null -ne $root)
        {
            try
            {
                $stopButton = Find-NamedElement $root ([System.Windows.Automation.ControlType]::Button) '停止广播'
                if ($null -ne $stopButton -and $stopButton.Current.IsEnabled)
                {
                    Invoke-Button $root '停止广播'
                    Start-Sleep -Seconds 2
                }
            }
            catch
            {
                $evidence.errors.Add("Cleanup Stop failed: $($_.Exception.Message)")
            }
        }
        if ($qtWindow -ne [IntPtr]::Zero)
        {
            [void][PixelBridgeGuiSmokeNative]::PostMessage($qtWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            [void]$encoderProcess.WaitForExit(10000)
        }
        if (-not $encoderProcess.HasExited)
        {
            Stop-Process -Id $encoderProcess.Id -Force
            $encoderProcess.WaitForExit()
            $evidence.errors.Add('Encoder GUI required forced cleanup after bounded graceful close')
        }
    }

    $registryKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($registrySubKey, $true)
    if ($null -eq $registryKey)
    {
        throw "Unable to reopen writable QSettings registry key: $registryPath"
    }
    foreach ($settingName in $settingsToPreserve)
    {
        $backup = $settingsBackup[$settingName]
        if ($backup.exists)
        {
            $registryKey.SetValue($settingName, $backup.value, $backup.kind)
        }
        else
        {
            $registryKey.DeleteValue($settingName, $false)
        }
    }
    $remainingNames = @($registryKey.GetValueNames())
    $registryKey.Close()
    if (-not $registryExisted -and $remainingNames.Count -eq 0)
    {
        Remove-Item -LiteralPath $registryPath
    }
    $evidence.settingsRestored = $true
}

$utf8 = [Text.UTF8Encoding]::new($false)
if ($null -eq $failure -and $evidence.errors.Count -ne 0)
{
    $evidence.status = 'FAIL'
    $failure = [Management.Automation.RuntimeException]::new($evidence.errors[0])
}
[IO.File]::WriteAllText($summaryPath, ($evidence | ConvertTo-Json -Depth 12) + [Environment]::NewLine, $utf8)
if ($null -ne $failure)
{
    throw "PB_ENCODER_GUI_LOCAL_DESKTOP_SMOKE_FAIL evidence=$summaryPath`n$($failure.Exception.Message)"
}
Write-Host "PB_ENCODER_GUI_LOCAL_DESKTOP_SMOKE_PASS summary=$summaryPath"

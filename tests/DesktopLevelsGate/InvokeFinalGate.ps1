#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../..'),
    [string]$VcpkgRoot = 'D:/vcpkg',
    [Parameter(Mandatory)][string]$PythonExecutable,
    [Parameter(Mandatory)][string]$CppcheckExecutable,
    [ValidateRange(1,16)][int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'StaticReview.ps1')
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$PythonExecutable = (Get-Command $PythonExecutable -ErrorAction Stop).Source
$CppcheckExecutable = (Get-Command $CppcheckExecutable -ErrorAction Stop).Source
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = (Get-Command ctest -ErrorAction Stop).Source
$git = (Get-Command git -ErrorAction Stop).Source
$script:steps = [Collections.Generic.List[object]]::new()
$script:artifacts = @{}
$script:nativeResults = [Collections.Generic.List[object]]::new()
$script:stage = 'precondition'
$script:passed = $true

function Write-NewJson([string]$Path, $Value)
{
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 24))
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) }
    finally { $file.Dispose() }
}

function Get-SourceIdentity
{
    # Git can return empty output while the system is shutting down; an
    # unguarded .Trim() there turns a shutdown into an unrecognizable
    # "method on null" failure, so normalize to '' before any method call.
    $headOutput = & $git -C $SourceRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository HEAD' }
    $head = ''
    if ($null -ne $headOutput) { $head = (@($headOutput) | Select-Object -Last 1).Trim() }
    if ($head.Length -eq 0) { throw 'Cannot resolve repository HEAD: git returned no output (git unavailable or the system is shutting down)' }
    $paths = @(& $git -C $SourceRoot -c core.quotePath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
    if ($LASTEXITCODE -ne 0 -or $paths.Count -eq 0) { throw 'Cannot inventory source files' }
    $files = @(foreach ($path in $paths)
    {
        $absolute = Join-Path $SourceRoot $path
        [ordered]@{ Path=$path; SHA256=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash }
    })
    $text = $files | ConvertTo-Json -Compress -Depth 4
    $digest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text)))
    return [pscustomobject]@{ Head=$head; Fingerprint=$digest; Files=$files }
}

function Assert-SystemStable([int]$MinimumUptimeSeconds)
{
    # Fail-closed preflight: a scheduled system restart (e.g. a Windows
    # Update restart) kills the long-running native capture tests and aborts
    # the Gate without complete evidence, so refuse to start while a restart
    # indicator is set or the system has only just booted.
    $problems = [Collections.Generic.List[string]]::new()
    $pendingRename = $null
    $sessionManagerKey = Get-ItemProperty -Path 'HKLM:/SYSTEM/CurrentControlSet/Control/Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
    if ($null -ne $sessionManagerKey) { $pendingRename = $sessionManagerKey.PendingFileRenameOperations }
    if ($null -ne $pendingRename -and @($pendingRename).Count -gt 0) { $problems.Add('PendingFileRenameOperations is set (system restart pending)') }
    $wuRebootRequired = $null
    $windowsUpdateKey = Get-ItemProperty -Path 'HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/WindowsUpdate/Auto Update' -Name RebootRequired -ErrorAction SilentlyContinue
    if ($null -ne $windowsUpdateKey) { $wuRebootRequired = $windowsUpdateKey.RebootRequired }
    if ($wuRebootRequired -eq 1) { $problems.Add('WindowsUpdate RebootRequired is set (system restart pending)') }
    $cbsRebootRequired = $null
    $cbsKey = Get-ItemProperty -Path 'HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/Component Based Servicing' -Name RebootRequired -ErrorAction SilentlyContinue
    if ($null -ne $cbsKey) { $cbsRebootRequired = $cbsKey.RebootRequired }
    if ($cbsRebootRequired -eq 1) { $problems.Add('Component Based Servicing RebootRequired is set (system restart pending)') }
    $uptimeSeconds = [Math]::Floor([Environment]::TickCount / 1000)
    if ($uptimeSeconds -lt $MinimumUptimeSeconds) { $problems.Add("system uptime ${uptimeSeconds}s is below the ${MinimumUptimeSeconds}s stability margin after a boot") }
    if ($problems.Count -gt 0)
    {
        throw "System is not stable for the long-running Gate: $($problems -join '; '). Complete or cancel the pending restart and rerun the Gate on a settled system."
    }
}

function Assert-Identity
{
    $current = Get-SourceIdentity
    if ($current.Head -cne $script:initial.Head -or $current.Fingerprint -cne $script:initial.Fingerprint)
    {
        throw 'Source/HEAD changed during Gate; retain evidence and rerun from a stable source state'
    }
    foreach ($entry in $script:artifacts.GetEnumerator())
    {
        if ((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -cne $entry.Value)
        {
            throw "Tested configuration/binary changed during Gate: $($entry.Key)"
        }
    }
}

function Invoke-Gate([string]$Name, [string]$Executable, [string[]]$Arguments, [int]$TimeoutSeconds = 1800, [int[]]$AllowedExitCodes = @(0))
{
    $script:stage = $Name
    Assert-Identity
    $prefix = Join-Path $script:evidence ('{0:D3}-{1}' -f $script:steps.Count,$Name)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = $SourceRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "DESKTOP_LEVELS_GATE_START $Name"
    if (-not $process.Start()) { throw "Cannot start $Executable" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $exitCode = $process.ExitCode
    $process.Dispose()
    $watch.Stop()
    [IO.File]::WriteAllText($prefix + '.stdout.log', $stdout, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($prefix + '.stderr.log', $stderr, [Text.UTF8Encoding]::new($false))
    $sanitizer = ($stdout + $stderr) -match 'AddressSanitizer:|ERROR:.*Sanitizer|SUMMARY:.*Sanitizer|runtime error:'
    $success = $exitCode -in $AllowedExitCodes -and -not $timedOut -and -not $sanitizer
    $record = [pscustomobject]@{ Name=$Name; Executable=$Executable; Arguments=@($Arguments); ExitCode=$exitCode;
        Passed=$success; TimedOut=$timedOut; SanitizerDiagnostic=$sanitizer; ElapsedMilliseconds=$watch.ElapsedMilliseconds;
        Stdout=$prefix + '.stdout.log'; Stderr=$prefix + '.stderr.log' }
    $script:steps.Add($record)
    if (-not $success) { $script:passed = $false }
    Assert-Identity
    Write-Host "DESKTOP_LEVELS_GATE_END $Name passed=$success exit=$exitCode ms=$($watch.ElapsedMilliseconds)"
    return $record
}

function Assert-CppcheckReview($Command)
{
    $review = Get-CppcheckReview $Command.Stderr $Command.ExitCode $SourceRoot (Join-Path $PSScriptRoot 'cppcheck_review.json')
    Write-NewJson ($Command.Stderr + '.review.json') $review
}

function Register-Build([string]$Root, [bool]$Asan)
{
    $cachePath = Join-Path $Root 'CMakeCache.txt'
    $cache = Get-Content -LiteralPath $cachePath -Raw
    foreach ($option in @('BUILD_TESTING','PB_BUILD_TESTS','PB_BUILD_APPS','PB_BUILD_TOOLS','PB_BUILD_DESKTOP_LEVELS_GATE',
        'PB_BUILD_LOCAL_DESKTOP_GATE','PB_BUILD_WGC_GATE','PB_BUILD_DXGI_GATE','PB_BUILD_PHASE0_GATE','PB_TREAT_WARNINGS_AS_ERRORS'))
    {
        if ($cache -notmatch "(?m)^${option}:BOOL=ON\r?$") { throw "Required option missing: $option" }
    }
    $fuzz = if ($Asan) { 'ON' } else { 'OFF' }
    if ($cache -notmatch "(?m)^PB_BUILD_FUZZERS:BOOL=$fuzz\r?$" -or
        $cache -notmatch '(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\r?$' -or
        $cache -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\r?$') { throw 'Incorrect declared compiler/sanitizer configuration' }
    foreach ($project in @('libs/PBModulation/PBModulation','libs/PBDesktopLevelsReference/PBDesktopLevelsReference',
        'libs/PBInnerFec/PBInnerFec','libs/PBCaptureNormalize/PBCaptureNormalize','libs/PBDemodD3D11/PBDemodD3D11',
        'libs/PBTelemetry/PBTelemetry','libs/PBRealCaptureReplay/PBRealCaptureReplay','apps/PixelBridgeEncoder/PixelBridgeEncoder',
        'apps/PixelBridgeDecoder/PixelBridgeDecoder','tests/PBModulation/PBDesktopLevelsTests','tests/PBModulation/PBShapeChromaTests',
        'tests/PBDemodD3D11/PBDemodD3D11Tests','tests/PBTelemetry/PBTelemetryTests','tests/PBRealCaptureReplay/PBRealCaptureReplayTests'))
    {
        $path = Join-Path $Root ($project + '.vcxproj')
        $content = Get-Content -LiteralPath $path -Raw
        if ($content -notmatch '<WarningLevel>Level4</WarningLevel>' -or $content -notmatch '<TreatWarningAsError>true</TreatWarningAsError>' -or
            ($Asan -and $content -notmatch '/fsanitize=address')) { throw "Required warning/ASan flags missing: $path" }
        $script:artifacts[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $script:artifacts[$cachePath] = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash
    # Scratch subproject executables generated by CTest are not part of this
    # build identity. Real applications, linked DLLs and direct test binaries are.
    foreach ($directory in @('apps','libs','tools','fuzz','tests'))
    {
        $path = Join-Path $Root $directory
        if (-not (Test-Path -LiteralPath $path)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object {
            $_.Directory.Name -eq 'Release' -and $_.Extension -in @('.exe','.dll') -and $_.FullName -notmatch 'isolation-|StaticLibraryIsolation-build|SubprojectDefault-build' })
        {
            $script:artifacts[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
    }
    $flavor = if ($Asan) { 'asan' } else { 'release' }
    Copy-Item -LiteralPath $cachePath -Destination (Join-Path $script:evidence "$flavor-CMakeCache.txt")
    Copy-Item -LiteralPath (Join-Path $Root 'vcpkg_installed/vcpkg/status') -Destination (Join-Path $script:evidence "$flavor-dependencies.txt")
}

try
{
    if ($env:ASAN_OPTIONS) { throw 'Gate requires default ASan options, not suppressed diagnostics' }
    Assert-SystemStable 600
    $script:initial = Get-SourceIdentity
    $script:evidence = Join-Path $SourceRoot ("build-desktop-levels-evidence/{0}-{1}-{2}" -f $script:initial.Head,
        (Get-Date -Format 'yyyyMMdd-HHmmss'),([Guid]::NewGuid().ToString('N')))
    [IO.Directory]::CreateDirectory($script:evidence) | Out-Null
    Write-NewJson (Join-Path $script:evidence 'source-identity.json') $script:initial
    Write-NewJson (Join-Path $script:evidence 'worktree.json') @(& $git -C $SourceRoot status --porcelain=v1 --untracked-files=all)
    $null = Invoke-Gate 'cmake-version' $cmake @('--version')
    $null = Invoke-Gate 'python-version' $PythonExecutable @('--version')
    $null = Invoke-Gate 'cppcheck-version' $CppcheckExecutable @('--version')
    $null = Invoke-Gate 'diff-whitespace' $git @('-C',$SourceRoot,'diff','--check')
    $null = Invoke-Gate 'old-bootstrap-oracle' $PythonExecutable @('tests/PBModulation/generate_local_desktop_golden.py','--check')
    $null = Invoke-Gate 'desktop-levels-oracle' $PythonExecutable @('tests/PBModulation/generate_desktop_levels_golden.py','--check')
    $common = @('-S',$SourceRoot,'-G','Visual Studio 17 2022','-A','x64',"-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
        '-DVCPKG_TARGET_TRIPLET=x64-windows','-DBUILD_TESTING=ON','-DPB_BUILD_TESTS=ON','-DPB_BUILD_APPS=ON','-DPB_BUILD_TOOLS=ON',
        '-DPB_BUILD_DESKTOP_LEVELS_GATE=ON','-DPB_BUILD_LOCAL_DESKTOP_GATE=ON','-DPB_BUILD_WGC_GATE=ON','-DPB_BUILD_DXGI_GATE=ON',
        '-DPB_BUILD_PHASE0_GATE=ON','-DPB_TREAT_WARNINGS_AS_ERRORS=ON','-DPB_BUILD_BENCHMARKS=OFF',
        '-DPB_BUILD_PRESENTATION_GATE=OFF','-DPB_BUILD_SCREEN_REGION_GATE=OFF')
    foreach ($flavor in @('release','asan'))
    {
        $build = Join-Path $SourceRoot "build-desktop-levels-$flavor"
        $fuzz = if ($flavor -eq 'asan') { 'ON' } else { 'OFF' }
        $configured = Invoke-Gate "$flavor-configure" $cmake ($common + @('-B',$build,"-DPB_BUILD_FUZZERS=$fuzz"))
        if (-not $configured.Passed) { continue }
        $built = Invoke-Gate "$flavor-default-build" $cmake @('--build',$build,'--config','Release','--parallel',"$Parallel") 2400
        if (-not $built.Passed) { continue }
        Register-Build $build ($flavor -eq 'asan')
        $selected = '^(PBDesktopLevelsTests|PBShapeChromaTests|PBDemodD3D11Tests|PBTelemetryTests|PBRealCaptureReplayTests|PBDesktopLevelsBaselineJson|PBModulationTests|PBLocalDesktopBootstrapTests|PBLocalDesktopMatrixTests|PBLocalDesktopNoAllocationProbe|PBInnerFecTests|PBInterleaveTests|PBBootstrapDiagnosticTests|PBCapturePipelineTests|PBCaptureRotationTests|PBDecoderCliTests|PBEncoderCliTests|PBDecoderTelemetryJson|PBCaptureNormalizeTests|PBGoldenVectorTests|PBGoldenVectorCheckTests)$'
        $null = Invoke-Gate "$flavor-related-ctest" $ctest @('--test-dir',$build,'--build-config','Release','--output-on-failure',
            '--parallel',"$Parallel",'-R',$selected,'--output-junit',(Join-Path $script:evidence "$flavor-related.xml"))
        $nativeRoot = Join-Path $build 'tests/DesktopLevelsGate/Release/evidence'
        $previousReports = @{}
        if (Test-Path -LiteralPath $nativeRoot)
        {
            foreach ($file in Get-ChildItem -LiteralPath $nativeRoot -Recurse -File -Filter report.json) { $previousReports[$file.FullName] = $true }
        }
        # Full CTest includes the four 30-second application measurements,
        # legacy/new Golden, actual recreation/scale rejection, and ASan corpus.
        # Failures are retained; there is no exclusion, skip, or retry filter.
        # The native loop selects its verified-frame/phase demand from the
        # build flavor: release keeps the full 16-phase certification demand,
        # asan keeps the documented baseline demand.
        Set-Item -Path Env:PB_DESKTOP_LEVELS_NATIVE_MODE -Value $flavor
        $null = Invoke-Gate "$flavor-all-ctest" $ctest @('--test-dir',$build,'--build-config','Release','--output-on-failure',
            '--parallel',"$Parallel",'--output-junit',(Join-Path $script:evidence "$flavor-all.xml")) 9000
        Remove-Item -Path Env:PB_DESKTOP_LEVELS_NATIVE_MODE -ErrorAction SilentlyContinue
        $reports = @(Get-ChildItem -LiteralPath $nativeRoot -Recurse -File -Filter report.json | Where-Object { -not $previousReports.ContainsKey($_.FullName) })
        if ($reports.Count -ne 4) { $script:passed = $false }
        foreach ($file in $reports)
        {
            $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if ($report.Gate -ne 'PASS') { $script:passed = $false }
            $script:nativeResults.Add([ordered]@{ Configuration=$flavor; ReportPath=$file.FullName; Result=$report })
        }
        $null = Invoke-Gate "$flavor-cpu-baseline" (Join-Path $build 'tools/Release/PBDesktopLevelsBaseline.exe') @('--baseline')
        if ($flavor -eq 'asan')
        {
            $null = Invoke-Gate 'asan-desktop-levels-mutation' (Join-Path $build 'fuzz/Release/PBDesktopLevelsMutation.exe') @('384')
        }
    }
    $staticProjects = @('libs/PBInterleave/PBInterleave','libs/PBModulation/PBModulation','libs/PBDesktopLevelsReference/PBDesktopLevelsReference',
        'libs/PBCaptureNormalize/PBCaptureNormalize','libs/PBDemodD3D11/PBDemodD3D11','libs/PBTelemetry/PBTelemetry',
        'libs/PBRealCaptureReplay/PBRealCaptureReplay','apps/PixelBridgeEncoder/PixelBridgeEncoder','apps/PixelBridgeDecoder/PixelBridgeDecoder',
        'tools/PBDesktopLevelsBaseline','fuzz/PBDesktopLevelsMutation')
    foreach ($project in $staticProjects)
    {
        $build = if ($project.StartsWith('fuzz/')) { 'build-desktop-levels-asan' } else { 'build-desktop-levels-release' }
        $name = ($project -split '/')[-1]
        $checked = Invoke-Gate "cppcheck-$name" $CppcheckExecutable @("--project=$(Join-Path $SourceRoot "$build/$project.vcxproj")",
            '--language=c++','--std=c++20','--platform=win64','--check-level=exhaustive','--inconclusive',
            '--enable=warning,style,performance,portability','--suppress=missingIncludeSystem','--xml','--xml-version=2','--error-exitcode=2') -AllowedExitCodes @(0,2)
        Assert-CppcheckReview $checked
    }
    Assert-Identity
}
catch
{
    $script:passed = $false
    Write-Error -ErrorAction Continue $_
    if (Test-Path variable:script:evidence)
    {
        Write-NewJson (Join-Path $script:evidence 'fatal-error.json') @{ Stage=$script:stage; Error=$_.Exception.Message }
    }
}
finally
{
    if (Test-Path variable:script:evidence)
    {
        Write-NewJson (Join-Path $script:evidence 'commands.json') @($script:steps)
        Write-NewJson (Join-Path $script:evidence 'native-comparison.json') @($script:nativeResults)
        Write-NewJson (Join-Path $script:evidence 'tested-artifacts.json') @($script:artifacts.GetEnumerator() | Sort-Object Key | ForEach-Object {
            [ordered]@{ Path=$_.Key; SHA256=$_.Value }
        })
        $status = if ($script:passed) { 'PASS' } else { 'FAIL' }
        Write-NewJson (Join-Path $script:evidence 'summary.json') ([ordered]@{ ExecutionGate=$status; Head=$script:initial.Head;
            SourceFingerprint=$script:initial.Fingerprint; NativeGroups=$script:nativeResults.Count; Commands=$script:steps.Count;
            CertifiedProfile=$false; FinalSystemComplete=$false; CommitCreated=$false;
            Decision='Execution evidence only; final diff/static review is required before a task-scoped atomic commit' })
        Write-Host "DESKTOP_LEVELS_GATE $status evidence=$script:evidence"
    }
}
if (-not $script:passed) { exit 1 }
exit 0

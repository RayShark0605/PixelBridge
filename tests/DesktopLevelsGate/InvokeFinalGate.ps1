#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../..'),
    [string]$VcpkgRoot = 'D:/vcpkg',
    [Parameter(Mandatory)][string]$QtRoot,
    [Parameter(Mandatory)][string]$PythonExecutable,
    [Parameter(Mandatory)][string]$CppcheckExecutable,
    [Parameter(Mandatory)][int]$MonitorOriginX,
    [Parameter(Mandatory)][int]$MonitorOriginY,
    [ValidateRange(1,16)][int]$Parallel = 4,
    [ValidateRange(300,1680)][UInt32]$Phase1SoakSeconds = 300
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'StaticReview.ps1')
. (Join-Path $PSScriptRoot 'GateJson.ps1')
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$PythonExecutable = (Get-Command $PythonExecutable -ErrorAction Stop).Source
$CppcheckExecutable = (Get-Command $CppcheckExecutable -ErrorAction Stop).Source
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = (Get-Command ctest -ErrorAction Stop).Source
$git = (Get-Command git -ErrorAction Stop).Source
$powershell = (Get-Command pwsh -ErrorAction Stop).Source
$script:steps = [Collections.Generic.List[object]]::new()
$script:artifacts = @{}
$script:nativeResults = [Collections.Generic.List[object]]::new()
$script:fileResults = [Collections.Generic.List[object]]::new()
$script:soakResults = [Collections.Generic.List[object]]::new()
$script:stage = 'precondition'
$script:passed = $true

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
    # Some systems expose an empty REG_MULTI_SZ as a one-element array whose
    # only element is $null. Count only actual source/destination strings; a
    # real pending rename always retains at least its nonempty source entry.
    $pendingRenameEntries = @($pendingRename | Where-Object { -not [string]::IsNullOrEmpty([string]$_) })
    if ($pendingRenameEntries.Count -gt 0) { $problems.Add('PendingFileRenameOperations is set (system restart pending)') }
    if (Test-Path -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/WindowsUpdate/Auto Update/RebootRequired')
    {
        $problems.Add('WindowsUpdate RebootRequired key exists (system restart pending)')
    }
    if (Test-Path -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/Component Based Servicing/RebootPending')
    {
        $problems.Add('Component Based Servicing RebootPending key exists (system restart pending)')
    }
    $uptimeSeconds = [Math]::Floor([Environment]::TickCount64 / 1000)
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

function Preserve-Phase0Scratch([string]$Build, [string]$Flavor)
{
    $buildRoot = [IO.Path]::GetFullPath($Build).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $phase0Root = [IO.Path]::GetFullPath((Join-Path $Build 'tests/Phase0Gate/Release')).TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    if (-not $phase0Root.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Phase 0 scratch root escaped the intended build tree'
    }
    $evidenceRoot = [IO.Path]::GetFullPath($script:evidence).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $archiveRoot = [IO.Path]::GetFullPath((Join-Path $script:evidence "preexisting-phase0-scratch/$Flavor"))
    if (-not $archiveRoot.StartsWith($evidenceRoot, [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Phase 0 scratch archive escaped the current evidence root'
    }
    $candidates = @('fast','large','resume','scratch-safety','phase0-fast.jsonl','phase0-large.jsonl',
        'phase0-resume.jsonl','phase0-scratch-safety.jsonl','phase0-resume-regressions.jsonl')
    foreach ($name in $candidates)
    {
        $source = [IO.Path]::GetFullPath((Join-Path $phase0Root $name))
        if (-not $source.StartsWith($phase0Root, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $source))
        {
            continue
        }
        [IO.Directory]::CreateDirectory($archiveRoot) | Out-Null
        $destination = Join-Path $archiveRoot ("{0}-{1}" -f $name,([Guid]::NewGuid().ToString('N')))
        Move-Item -LiteralPath $source -Destination $destination
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
    $expectedOriginX = [regex]::Escape([string]$MonitorOriginX)
    $expectedOriginY = [regex]::Escape([string]$MonitorOriginY)
    if ($cache -notmatch "(?m)^PB_DESKTOP_TEST_MONITOR_ORIGIN_X:STRING=$expectedOriginX\r?$" -or
        $cache -notmatch "(?m)^PB_DESKTOP_TEST_MONITOR_ORIGIN_Y:STRING=$expectedOriginY\r?$")
    {
        throw 'Configured native test monitor origin differs from the requested physical desktop origin'
    }
    foreach ($project in @('libs/PBModulation/PBModulation','libs/PBDesktopLevelsReference/PBDesktopLevelsReference',
        'libs/PBInnerFec/PBInnerFec','libs/PBCaptureNormalize/PBCaptureNormalize','libs/PBDemodD3D11/PBDemodD3D11',
        'libs/PBTelemetry/PBTelemetry','libs/PBRealCaptureReplay/PBRealCaptureReplay','libs/PBStorage/PBStorage',
        'apps/common/PBApplication','apps/PixelBridgeEncoder/PixelBridgeEncoder',
        'apps/PixelBridgeDecoder/PixelBridgeDecoder','tests/PBModulation/PBDesktopLevelsTests','tests/PBModulation/PBShapeChromaTests',
        'tests/PBDemodD3D11/PBDemodD3D11Tests','tests/PBTelemetry/PBTelemetryTests','tests/PBRealCaptureReplay/PBRealCaptureReplayTests',
        'tests/PBApplication/PBApplicationTests','tests/PBApplication/PBQSettingsTests','tests/PBStorage/PBStorageTests',
        'tests/DesktopLevelsGate/PBPhase1FileGate','tests/DesktopLevelsGate/PBDesktopLevelsNativeSupport',
        'tests/DesktopLevelsGate/PBPhase1FileGateArgumentsTests'))
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
    # Keep every nested source/output/.part path below legacy MAX_PATH even on
    # hosts where the long-path policy is disabled. HEAD remains sealed in
    # source-identity.json; it need not be duplicated in the directory name.
    $runToken = [Guid]::NewGuid().ToString('N').Substring(0,12)
    $script:evidence = Join-Path $SourceRoot ("build-desktop-levels-evidence/phase1-{0}-{1}" -f
        (Get-Date -Format 'yyyyMMdd-HHmmss'),$runToken)
    $maximumNestedPath = Join-Path $script:evidence ("phase1-long-soak/{0}/accepted.bin.part" -f ('x' * 96))
    if ($maximumNestedPath.Length -ge 260)
    {
        throw 'Phase 1 evidence root is too deep for the legacy Win32 storage path used by the physical file Gate'
    }
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
        "-DPB_QT_ROOT=$QtRoot",
        '-DVCPKG_TARGET_TRIPLET=x64-windows','-DBUILD_TESTING=ON','-DPB_BUILD_TESTS=ON','-DPB_BUILD_APPS=ON','-DPB_BUILD_TOOLS=ON',
        '-DPB_BUILD_DESKTOP_LEVELS_GATE=ON','-DPB_BUILD_LOCAL_DESKTOP_GATE=ON','-DPB_BUILD_WGC_GATE=ON','-DPB_BUILD_DXGI_GATE=ON',
        '-DPB_BUILD_PHASE0_GATE=ON','-DPB_TREAT_WARNINGS_AS_ERRORS=ON','-DPB_BUILD_BENCHMARKS=OFF',
        '-DPB_BUILD_PRESENTATION_GATE=OFF','-DPB_BUILD_SCREEN_REGION_GATE=OFF',
        "-DPB_DESKTOP_TEST_MONITOR_ORIGIN_X=$MonitorOriginX","-DPB_DESKTOP_TEST_MONITOR_ORIGIN_Y=$MonitorOriginY")
    foreach ($flavor in @('release','asan'))
    {
        $build = Join-Path $SourceRoot "build-desktop-levels-$flavor"
        $fuzz = if ($flavor -eq 'asan') { 'ON' } else { 'OFF' }
        $configured = Invoke-Gate "$flavor-configure" $cmake ($common + @('-B',$build,"-DPB_BUILD_FUZZERS=$fuzz"))
        if (-not $configured.Passed) { continue }
        $built = Invoke-Gate "$flavor-default-build" $cmake @('--build',$build,'--config','Release','--parallel',"$Parallel") 2400
        if (-not $built.Passed) { continue }
        Register-Build $build ($flavor -eq 'asan')
        $selected = '^(PBDesktopLevelsTests|PBShapeChromaTests|PBDemodD3D11Tests|PBTelemetryTests|PBRealCaptureReplayTests|PBDesktopLevelsBaselineJson|PBModulationTests|PBLocalDesktopBootstrapTests|PBLocalDesktopMatrixTests|PBLocalDesktopNoAllocationProbe|PBInnerFecTests|PBInterleaveTests|PBBootstrapDiagnosticTests|PBCapturePipelineTests|PBCaptureRotationTests|PBDecoderCliTests|PBEncoderCliTests|PBDecoderTelemetryJson|PBCaptureNormalizeTests|PBGoldenVectorTests|PBGoldenVectorCheckTests|PBPhase1FileGatePolicyTests|PBPhase1FileResourcesTests|PBPhase1FileGateArgumentsTests)$'
        $null = Invoke-Gate "$flavor-related-ctest" $ctest @('--test-dir',$build,'--build-config','Release','--output-on-failure',
            '--parallel',"$Parallel",'-R',$selected,'--output-junit',(Join-Path $script:evidence "$flavor-related.xml"))
        $nativeRoot = Join-Path $build 'tests/DesktopLevelsGate/Release/evidence'
        $fileRoot = Join-Path $build 'tests/DesktopLevelsGate/Release/phase1-file-evidence'
        $previousReports = @{}
        $previousFileReports = @{}
        if (Test-Path -LiteralPath $nativeRoot)
        {
            foreach ($file in Get-ChildItem -LiteralPath $nativeRoot -Recurse -File -Filter report.json) { $previousReports[$file.FullName] = $true }
        }
        if (Test-Path -LiteralPath $fileRoot)
        {
            foreach ($file in Get-ChildItem -LiteralPath $fileRoot -Recurse -File -Filter phase1-file-report.json)
            {
                $previousFileReports[$file.FullName] = $true
            }
        }
        # Full CTest includes six 30-second application measurements (both
        # backends times the two Direct-Level candidates and ShapeChroma),
        # legacy/new Golden, actual recreation/scale rejection, and ASan corpus.
        # Failures are retained; there is no exclusion, skip, or retry filter.
        # The native loop selects its verified-frame/phase demand from the
        # build flavor: release keeps the full 16-phase certification demand,
        # asan keeps the documented baseline demand.
        Preserve-Phase0Scratch $build $flavor
        Set-Item -Path Env:PB_DESKTOP_LEVELS_NATIVE_MODE -Value $flavor
        $allCtest = Invoke-Gate "$flavor-all-ctest" $ctest @('--test-dir',$build,'--build-config','Release','--output-on-failure',
            '--parallel',"$Parallel",'--output-junit',(Join-Path $script:evidence "$flavor-all.xml")) 9000
        Remove-Item -Path Env:PB_DESKTOP_LEVELS_NATIVE_MODE -ErrorAction SilentlyContinue
        $reports = @(Get-ChildItem -LiteralPath $nativeRoot -Recurse -File -Filter report.json | Where-Object { -not $previousReports.ContainsKey($_.FullName) })
        if ($reports.Count -ne 6) { $script:passed = $false }
        $nativeMatrix = [Collections.Generic.HashSet[string]]::new()
        foreach ($file in $reports)
        {
            $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            $matrixKey = "$($report.Backend)/$($report.Candidate)"
            if (-not $nativeMatrix.Add($matrixKey) -or $report.Gate -cne 'PASS' -or
                [string]$report.Backend -notin @('wgc','dxgi') -or
                [string]$report.Candidate -notin @('desktop-levels-2x2','desktop-levels-4x4','shape-chroma'))
            {
                $script:passed = $false
            }
            $script:nativeResults.Add([ordered]@{ Configuration=$flavor; ReportPath=$file.FullName; Result=$report })
        }
        if ($nativeMatrix.Count -ne 6) { $script:passed = $false }
        $fileReports = @()
        if (Test-Path -LiteralPath $fileRoot)
        {
            $fileReports = @(Get-ChildItem -LiteralPath $fileRoot -Recurse -File -Filter phase1-file-report.json |
                Where-Object { -not $previousFileReports.ContainsKey($_.FullName) })
        }
        $regularFilePassed = $allCtest.Passed -and $fileReports.Count -eq 4
        $regularMatrix = [Collections.Generic.HashSet[string]]::new()
        foreach ($file in $fileReports)
        {
            $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            $matrixKey = "$($report.Backend)/$($report.Profile)"
            if (-not $regularMatrix.Add($matrixKey) -or $report.Gate -cne 'PASS' -or
                [string]$report.Backend -notin @('wgc','dxgi') -or
                [string]$report.Profile -notin @('desktop-levels-2x2','shape-chroma') -or
                [string]$report.NativeMode -cne $flavor -or
                [bool]$report.PerformanceCertification -ne ($flavor -ceq 'release') -or
                [UInt32]$report.RequestedSoakSeconds -ne 0)
            {
                $regularFilePassed = $false
            }
            $script:fileResults.Add([ordered]@{ Configuration=$flavor; ReportPath=$file.FullName; Result=$report })
        }
        if ($regularMatrix.Count -ne 4)
        {
            $regularFilePassed = $false
        }
        if (-not $regularFilePassed)
        {
            $script:passed = $false
        }
        if ($flavor -eq 'release' -and $regularFilePassed)
        {
            $soakRoot = Join-Path $script:evidence 'phase1-long-soak'
            foreach ($backend in @('wgc','dxgi'))
            {
                foreach ($profile in @('desktop-levels-2x2','shape-chroma'))
                {
                    $null = Invoke-Gate "release-phase1-soak-$backend-$profile" $powershell @('-NoProfile','-File',
                        (Join-Path $SourceRoot 'tests/DesktopLevelsGate/InvokePhase1FileLoop.ps1'),
                        '-GateExecutable',(Join-Path $build 'tests/DesktopLevelsGate/Release/PBPhase1FileGate.exe'),
                        '-Support',(Join-Path $build 'tests/DesktopLevelsGate/Release/PBDesktopLevelsNativeSupport.exe'),
                        '-Backend',$backend,'-Profile',$profile,'-EvidenceRoot',$soakRoot,
                        '-NativeMode','release','-MonitorOriginX',"$MonitorOriginX",'-MonitorOriginY',"$MonitorOriginY",
                        '-SoakSeconds',"$Phase1SoakSeconds") ([int]$Phase1SoakSeconds + 180)
                }
            }
            $soakReports = @(Get-ChildItem -LiteralPath $soakRoot -Recurse -File -Filter phase1-file-report.json)
            $soakMatrix = [Collections.Generic.HashSet[string]]::new()
            if ($soakReports.Count -ne 4)
            {
                $script:passed = $false
            }
            foreach ($file in $soakReports)
            {
                $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
                $matrixKey = "$($report.Backend)/$($report.Profile)"
                if (-not $soakMatrix.Add($matrixKey) -or $report.Gate -cne 'PASS' -or
                    [string]$report.Backend -notin @('wgc','dxgi') -or
                    [string]$report.Profile -notin @('desktop-levels-2x2','shape-chroma') -or
                    [string]$report.NativeMode -cne 'release' -or -not [bool]$report.PerformanceCertification -or
                    [UInt32]$report.RequestedSoakSeconds -ne $Phase1SoakSeconds -or -not [bool]$report.Resources.LongSoakVerified)
                {
                    $script:passed = $false
                }
                $script:soakResults.Add([ordered]@{ Configuration=$flavor; ReportPath=$file.FullName; Result=$report })
            }
            if ($soakMatrix.Count -ne 4)
            {
                $script:passed = $false
            }
        }
        $null = Invoke-Gate "$flavor-cpu-baseline" (Join-Path $build 'tools/Release/PBDesktopLevelsBaseline.exe') @('--baseline')
        if ($flavor -eq 'asan')
        {
            $null = Invoke-Gate 'asan-desktop-levels-mutation' (Join-Path $build 'fuzz/Release/PBDesktopLevelsMutation.exe') @('384')
        }
    }
    $staticProjects = @('libs/PBInterleave/PBInterleave','libs/PBModulation/PBModulation','libs/PBDesktopLevelsReference/PBDesktopLevelsReference',
        'libs/PBCaptureNormalize/PBCaptureNormalize','libs/PBDemodD3D11/PBDemodD3D11','libs/PBTelemetry/PBTelemetry',
        'libs/PBRealCaptureReplay/PBRealCaptureReplay','libs/PBStorage/PBStorage','apps/common/PBApplication',
        'apps/PixelBridgeEncoder/PixelBridgeEncoder','apps/PixelBridgeDecoder/PixelBridgeDecoder',
        'tests/DesktopLevelsGate/PBPhase1FileGate','tests/DesktopLevelsGate/PBDesktopLevelsNativeSupport',
        'tools/PBDesktopLevelsBaseline','fuzz/PBDesktopLevelsMutation',
        # Remote-visual low-FPS reference stack (Step 02/05/06/07): the simulator library and every
        # evidence tool that produces corpus, receiver-truth or real-Replay classification artifacts
        # are reviewed here. The presenter is also part of the Step 02 evidence provenance chain.
        'libs/PBRemoteVisualSimulator/PBRemoteVisualSimulator',
        'tools/PBRemoteVisualChannelMatrixCore','tools/PBRemoteVisualChannelMatrix',
        'tools/PBRemoteVisualCodecProbeCore','tools/PBRemoteVisualCodecProbe',
        'tools/PBRemoteVisualReceiverEvidenceCore',
        'tools/PBRemoteVisualTemporalCorpusCore','tools/PBRemoteVisualTemporalCorpus',
        'tools/PBRemoteVisualReplayInspectorCore','tools/PBRemoteVisualReplayInspector',
        'tools/PBRemoteVisualEvidencePresenter',
        'tools/PBRemoteVisualMetricCalibrationCore','tools/PBRemoteVisualMetricCalibration')
    foreach ($project in $staticProjects)
    {
        $build = if ($project.StartsWith('fuzz/')) { 'build-desktop-levels-asan' } else { 'build-desktop-levels-release' }
        $name = ($project -split '/')[-1]
        $projectFile = Join-Path $SourceRoot "$build/$project.vcxproj"
        $cppcheckArguments = @("--project=$projectFile",'--project-configuration=Release|x64',
            '--language=c++','--std=c++20','--platform=win64','--check-level=exhaustive','--inconclusive',
            '--enable=warning,style,performance,portability','--suppress=missingIncludeSystem','--xml','--xml-version=2','--error-exitcode=2')
        if ($project.StartsWith('apps/'))
        {
            $cppcheckArguments += @('-Dslots=','-Dsignals=public','-DQ_OBJECT=','-Demit=')
            $autogenDirectory = Join-Path (Split-Path -Parent $projectFile) "${name}_autogen"
            if (Test-Path -LiteralPath $autogenDirectory)
            {
                $cppcheckArguments += "-i$autogenDirectory"
            }
        }
        $checked = Invoke-Gate "cppcheck-$name" $CppcheckExecutable $cppcheckArguments -AllowedExitCodes @(0,2)
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
        Write-NewJson (Join-Path $script:evidence 'phase1-file-regular.json') @($script:fileResults)
        Write-NewJson (Join-Path $script:evidence 'phase1-file-soak.json') @($script:soakResults)
        Write-NewJson (Join-Path $script:evidence 'tested-artifacts.json') @($script:artifacts.GetEnumerator() | Sort-Object Key | ForEach-Object {
            [ordered]@{ Path=$_.Key; SHA256=$_.Value }
        })
        $status = if ($script:passed) { 'PASS' } else { 'FAIL' }
        $releaseRegular = @($script:fileResults | Where-Object { $_.Configuration -ceq 'release' })
        $successfulReleaseRegular = @($releaseRegular | Where-Object { Test-Phase1SuccessfulFileResult $_.Result })
        $profileBaselines = @(foreach ($profile in @('desktop-levels-2x2','shape-chroma'))
        {
            $runs = @($successfulReleaseRegular | Where-Object { $_.Result.Profile -ceq $profile })
            [ordered]@{
                Profile=$profile
                Runs=@($runs | ForEach-Object {
                    [ordered]@{
                        Backend=$_.Result.Backend
                        VerifiedEncodedGoodputBytesPerSecond=$_.Result.Receiver.verifiedEncodedGoodputBytesPerSecond
                        PostFecFER=$_.Result.Receiver.postFecFer
                        UniqueVisualFPS=$_.Result.Receiver.uniqueVisualFps
                        EndToEndUniqueVisualFPS=$_.Result.Receiver.endToEndUniqueVisualFps
                        UniqueVisualCadenceIntervals=$_.Result.Receiver.uniqueVisualCadenceIntervals
                        CaptureDeliveryRatio=$_.Result.Receiver.captureDeliveryRatio
                        RequiredPostPublishObservationSeconds=$_.Result.Receiver.requiredPostPublishObservationSeconds
                        PostPublishObservationSeconds=$_.Result.Receiver.postPublishObservationSeconds
                        RoiGpuAverageMilliseconds=$_.Result.Receiver.roiGpuAverageMilliseconds
                        DemodGpuAverageMilliseconds=$_.Result.Receiver.demodGpuAverageMilliseconds
                        BootstrapCpuAverageMilliseconds=$_.Result.Receiver.bootstrapCpuAverageMilliseconds
                        PostGpuFecCpuAverageMilliseconds=$_.Result.Receiver.postGpuFecCpuAverageMilliseconds
                        SenderEquivalentCpuCores=$_.Result.Resources.SenderEquivalentCpuCores
                        ReceiverEquivalentCpuCores=$_.Result.Resources.ReceiverEquivalentCpuCores
                    }
                })
            }
        })
        $primaryErrorModes = @($successfulReleaseRegular | ForEach-Object {
            [ordered]@{
                Backend=$_.Result.Backend
                Profile=$_.Result.Profile
                PostFecFailedFrames=$_.Result.Receiver.postFecFailedFrames
                BootstrapMismatchErasures=$_.Result.Receiver.bootstrapMismatchFrames
                ForeignSessionErasedFrames=$_.Result.Receiver.foreignSessionErasedFrames
                RetryableUnknownSessionControlDrops=$_.Result.Receiver.retryableUnknownSessionControlDrops
                UnboundSessionVisualFrames=$_.Result.Receiver.unboundSessionVisualFrames
                CaptureDrops=$_.Result.Receiver.captureDrops
                CaptureExpired=$_.Result.Receiver.captureExpired
                CaptureDeliveryRatio=$_.Result.Receiver.captureDeliveryRatio
                DuplicateVisualFrames=$_.Result.Receiver.duplicateVisualFrames
                UniqueVisualGapEvents=$_.Result.Receiver.uniqueVisualGapEvents
                SkippedVisualSequences=$_.Result.Receiver.skippedVisualSequences
                RoiGpuTimingUnavailable=$_.Result.Receiver.roiGpuTimingUnavailable
                DemodGpuTimingUnavailable=$_.Result.Receiver.demodGpuTimingUnavailable
            }
        })
        $tagAllowedAfterReview = $script:passed -and $script:nativeResults.Count -eq 12 -and
            $script:fileResults.Count -eq 8 -and $script:soakResults.Count -eq 4
        Write-NewJson (Join-Path $script:evidence 'phase1-comparison.json') ([ordered]@{
            Gate=$status
            PhysicalLayerBaselines=$profileBaselines
            PrimaryErrorModes=$primaryErrorModes
            LongSoakRuns=@($script:soakResults)
            MonitorOrigin=@($MonitorOriginX,$MonitorOriginY)
            Phase2Required=@(
                'Move fixed Bootstrap/Control extraction off the full 1920x1080 CPU readback path while preserving same-frame retirement proof',
                'Replace the literal experimental ShapeChroma A/B codebook with measured profile selection and a separately reviewed certified wire/profile baseline',
                'Extend the hardware matrix to actual adapter removal, hot-plug, HDR/SDR transitions, SDR duplication source-format/bit-depth changes and multi-monitor mode changes beyond deterministic fault injection and requested recreation',
                'Integrate the proven single-segment Gate path into the bounded production scheduler without duplicating Receiver, FEC, storage or capture architecture'
            )
            TagAllowedAfterDiffReview=$tagAllowedAfterReview
            CertifiedProfile=$false
            FinalSystemComplete=$false
        })
        Write-NewJson (Join-Path $script:evidence 'summary.json') ([ordered]@{ ExecutionGate=$status; Head=$script:initial.Head;
            SourceFingerprint=$script:initial.Fingerprint; NativeGroups=$script:nativeResults.Count;
            Phase1RegularFileGroups=$script:fileResults.Count; Phase1LongSoakGroups=$script:soakResults.Count;
            Commands=$script:steps.Count; Phase1TagAllowedAfterDiffReview=$tagAllowedAfterReview;
            MonitorOrigin=@($MonitorOriginX,$MonitorOriginY);
            CertifiedProfile=$false; FinalSystemComplete=$false; CommitCreated=$false;
            Decision='Execution evidence only; tag phase1-gate-pass is allowed only after PASS plus final scoped diff/static review and an atomic commit' })
        Write-Host "DESKTOP_LEVELS_GATE $status evidence=$script:evidence"
    }
}
if (-not $script:passed) { exit 1 }
exit 0

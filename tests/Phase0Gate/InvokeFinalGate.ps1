#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{40}$')][string]$ExpectedCommit,
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../..'),
    [string]$VcpkgRoot = 'D:/vcpkg',
    [Parameter(Mandatory = $true)][string]$CppcheckExecutable,
    [ValidateRange(1, 16)][int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$CppcheckExecutable = (Get-Command $CppcheckExecutable -ErrorAction Stop).Source
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = (Get-Command ctest -ErrorAction Stop).Source
$git = (Get-Command git -ErrorAction Stop).Source
$powershell = (Get-Process -Id $PID).Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $SourceRoot "build-phase0-gate-evidence/$ExpectedCommit-$stamp"
$script:identityFiles = @{}
$script:stageNumber = 0
$script:lastStage = 'precondition'

function Get-CleanHead
{
    # Normalize empty git output (e.g. system shutdown) before any method call.
    $headOutput = & $git -C $SourceRoot rev-parse HEAD
    $head = ''
    if ($null -ne $headOutput) { $head = (@($headOutput) | Select-Object -Last 1).Trim() }
    if ($LASTEXITCODE -ne 0 -or $head -cne $ExpectedCommit)
    {
        throw "HEAD differs from FINAL_PHASE0_COMMIT: $head"
    }
    $status = @(& $git -C $SourceRoot status --porcelain=v1 --untracked-files=all)
    if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0)
    {
        throw "Final Gate requires a clean worktree; no stash/reset is permitted: $($status -join '; ')"
    }
    return $head
}

function Write-Json($Value, [string]$Path)
{
    $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Assert-Identity([string]$Stage, [string]$Boundary)
{
    $head = Get-CleanHead
    foreach ($entry in $script:identityFiles.GetEnumerator())
    {
        if (-not (Test-Path -LiteralPath $entry.Key -PathType Leaf) -or
            (Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -cne $entry.Value)
        {
            throw "Configuration/test binary identity changed: $($entry.Key)"
        }
    }
    [ordered]@{ stage = $Stage; boundary = $Boundary; commit = $head; worktree = 'clean';
        verified_artifacts = $script:identityFiles.Count; utc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $evidenceRoot 'identity.jsonl') -Encoding utf8
}

function Invoke-GateCommand
{
    param([string]$Name, [string]$Executable, [string[]]$Arguments = @(),
        [int]$TimeoutSeconds = 1800, [int[]]$AllowedExitCodes = @(0))
    $script:lastStage = $Name
    Assert-Identity $Name 'before'
    $script:stageNumber++
    $prefix = Join-Path $evidenceRoot ('{0:D3}-{1}' -f $script:stageNumber, $Name)
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $SourceRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $startInfo.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "FINAL_GATE_START $Name commit=$ExpectedCommit"
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
    Set-Content -LiteralPath ($prefix + '.stdout.log') -Value $stdout -NoNewline -Encoding utf8
    Set-Content -LiteralPath ($prefix + '.stderr.log') -Value $stderr -NoNewline -Encoding utf8
    $sanitizer = ($stdout + $stderr) -match 'AddressSanitizer:|ERROR:.*Sanitizer|SUMMARY:.*Sanitizer|runtime error:'
    $record = [ordered]@{ name = $Name; commit = $ExpectedCommit; executable = $Executable;
        arguments = @($Arguments); working_directory = $SourceRoot; exit_code = $exitCode;
        timed_out = $timedOut; sanitizer_diagnostic = $sanitizer; elapsed_ms = $watch.ElapsedMilliseconds;
        stdout = $prefix + '.stdout.log'; stderr = $prefix + '.stderr.log' }
    $record | ConvertTo-Json -Compress -Depth 8 |
        Add-Content -LiteralPath (Join-Path $evidenceRoot 'commands.jsonl') -Encoding utf8
    Assert-Identity $Name 'after'
    if ($timedOut -or $exitCode -notin $AllowedExitCodes -or $sanitizer)
    {
        throw "Stage $Name failed: exit=$exitCode timeout=$timedOut sanitizer=$sanitizer; inspect $prefix"
    }
    Write-Host "FINAL_GATE_END $Name exit=$exitCode elapsed_ms=$($watch.ElapsedMilliseconds)"
    return [pscustomobject]@{ stdout = $stdout; stderr = $stderr; exit_code = $exitCode; prefix = $prefix }
}

function Register-BuildIdentity([string]$BuildRoot, [string]$Configuration, [bool]$Asan)
{
    $cachePath = Join-Path $BuildRoot 'CMakeCache.txt'
    $cache = Get-Content -LiteralPath $cachePath -Raw
    foreach ($option in @('BUILD_TESTING', 'PB_BUILD_TESTS', 'PB_BUILD_APPS', 'PB_BUILD_TOOLS', 'PB_BUILD_PHASE0_GATE', 'PB_TREAT_WARNINGS_AS_ERRORS'))
    {
        if ($cache -notmatch "(?m)^${option}:BOOL=ON\r?$") { throw "Required option is not ON: $option" }
    }
    if ($cache -notmatch '(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\r?$' -or
        $cache -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\r?$')
    {
        throw 'Final Gate requires the declared MSVC x64 generator'
    }
    $fuzzValue = if ($Asan) { 'ON' } else { 'OFF' }
    if ($cache -notmatch "(?m)^PB_BUILD_FUZZERS:BOOL=$fuzzValue\r?$" -or $cache -notmatch '(?m)^PB_BUILD_BENCHMARKS:BOOL=OFF\r?$')
    {
        throw 'Incorrect sanitizer/benchmark configuration'
    }
    $projects = @('libs/PBProtocol/PBProtocol', 'libs/PBCompression/PBCompression', 'libs/PBOuterFec/PBOuterFec',
        'libs/PBInnerFec/PBInnerFec', 'libs/PBModulation/PBModulation', 'libs/PBReceiver/PBReceiver', 'tests/Phase0Gate/PBPhase0Gate')
    foreach ($project in $projects)
    {
        $path = Join-Path $BuildRoot ($project + '.vcxproj')
        $projectText = Get-Content -LiteralPath $path -Raw
        if ($projectText -notmatch '<WarningLevel>Level4</WarningLevel>' -or
            $projectText -notmatch '<TreatWarningAsError>true</TreatWarningAsError>' -or
            ($Asan -and $projectText -notmatch '/fsanitize=address'))
        {
            throw "Compiler policy/instrumentation missing in $path"
        }
        $script:identityFiles[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $script:identityFiles[$cachePath] = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash
    $binaries = @(Get-ChildItem -LiteralPath $BuildRoot -Recurse -File -Include '*.exe', '*.dll' |
        Where-Object { $_.Directory.Name -eq $Configuration })
    if ($binaries.Count -eq 0) { throw "No binaries in $BuildRoot" }
    foreach ($binary in $binaries)
    {
        $script:identityFiles[$binary.FullName] = (Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash
    }
    $name = if ($Asan) { 'asan' } else { 'release' }
    Copy-Item -LiteralPath $cachePath -Destination (Join-Path $evidenceRoot "$name-CMakeCache.txt")
    Copy-Item -LiteralPath (Join-Path $BuildRoot 'vcpkg_installed/vcpkg/status') -Destination (Join-Path $evidenceRoot "$name-dependencies.txt")
    foreach ($compilerFile in Get-ChildItem -LiteralPath (Join-Path $BuildRoot 'CMakeFiles') -Recurse -Filter 'CMakeCXXCompiler.cmake' -File)
    {
        Copy-Item -LiteralPath $compilerFile.FullName -Destination (Join-Path $evidenceRoot "$name-compiler.cmake")
    }
}

function Check-JUnit([string]$Path)
{
    [xml]$document = Get-Content -LiteralPath $Path -Raw
    $suite = $document.testsuite
    if ($null -eq $suite -or [int]$suite.tests -le 0 -or [int]$suite.failures -ne 0 -or [int]$suite.errors -ne 0)
    {
        throw "JUnit contains failures or no tests: $Path"
    }
    return [ordered]@{ tests = [int]$suite.tests; failures = [int]$suite.failures;
        disabled = [int]$suite.disabled; junit = $Path }
}

function Compare-VectorTrees([string]$Left, [string]$Right)
{
    $leftFiles = @(Get-ChildItem -LiteralPath $Left -Recurse -File | Sort-Object FullName)
    $rightFiles = @(Get-ChildItem -LiteralPath $Right -Recurse -File | Sort-Object FullName)
    if ($leftFiles.Count -eq 0 -or $leftFiles.Count -ne $rightFiles.Count) { throw 'VectorGen inventory mismatch' }
    $results = foreach ($file in $leftFiles)
    {
        $relative = [IO.Path]::GetRelativePath($Left, $file.FullName)
        $peer = Join-Path $Right $relative
        if (-not (Test-Path -LiteralPath $peer -PathType Leaf)) { throw "Missing VectorGen peer: $relative" }
        $leftBytes = [IO.File]::ReadAllBytes($file.FullName)
        $rightBytes = [IO.File]::ReadAllBytes($peer)
        if (-not [System.Linq.Enumerable]::SequenceEqual[byte]($leftBytes, $rightBytes))
        {
            throw "VectorGen byte inequality: $relative"
        }
        [ordered]@{ path = $relative; bytes = $leftBytes.Length; byte_identical = $true;
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
    Write-Json ([ordered]@{ commit = $ExpectedCommit; seed = 12648430; files = @($results); status = 'pass' }) `
        (Join-Path $evidenceRoot 'vector-replay.json')
}

try
{
    $null = Get-CleanHead
    if ($env:ASAN_OPTIONS) { throw 'Final Gate requires unmodified default ASan options' }
    if (Test-Path -LiteralPath $evidenceRoot) { throw "Evidence directory already exists: $evidenceRoot" }
    [IO.Directory]::CreateDirectory($evidenceRoot) | Out-Null
    Set-Content -LiteralPath (Join-Path $SourceRoot 'build-phase0-gate-evidence/current-p015-final.txt') -Value $evidenceRoot -Encoding utf8
    $releaseBuild = Join-Path $SourceRoot "build-p015-$ExpectedCommit-$stamp-release"
    $asanBuild = Join-Path $SourceRoot "build-p015-$ExpectedCommit-$stamp-asan"
    foreach ($build in @($releaseBuild, $asanBuild))
    {
        if (Test-Path -LiteralPath $build) { throw "Final Gate cannot reuse build tree: $build" }
    }
    Write-Json ([ordered]@{ commit = $ExpectedCommit; source_root = $SourceRoot; timestamp = $stamp;
        os = [Environment]::OSVersion.VersionString; architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString();
        processors = [Environment]::ProcessorCount; powershell = $PSVersionTable.PSVersion.ToString();
        release_build = $releaseBuild; asan_build = $asanBuild; ASAN_OPTIONS = $env:ASAN_OPTIONS;
        large_asan = 'Not run under ASan by policy';
        instrumentation = 'MSVC deterministic mutation + ASan; not coverage-guided libFuzzer or UBSan' }) `
        (Join-Path $evidenceRoot 'environment.json')
    $trackedPaths = @(& $git -c core.quotepath=false -C $SourceRoot ls-files)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory tracked files' }
    $sourceHashes = foreach ($relative in $trackedPaths)
    {
        $path = Join-Path $SourceRoot $relative
        [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
    Write-Json @($sourceHashes) (Join-Path $evidenceRoot 'source-files-sha256.json')
    $null = Invoke-GateCommand 'git-diff-check' $git @('-C', $SourceRoot, 'diff', '--check')
    $null = Invoke-GateCommand 'commit-whitespace-check' $git @('-C', $SourceRoot, 'show', '--format=', '--check', $ExpectedCommit)
    $null = Invoke-GateCommand 'cmake-version' $cmake @('--version')
    $null = Invoke-GateCommand 'cppcheck-version' $CppcheckExecutable @('--version')
    $vcpkgIdentity = Invoke-GateCommand 'vcpkg-identity' $git @('-C', $VcpkgRoot, 'rev-parse', 'HEAD')
    $manifest = Get-Content -LiteralPath (Join-Path $SourceRoot 'vcpkg.json') -Raw | ConvertFrom-Json
    if ($vcpkgIdentity.stdout.Trim() -cne $manifest.'builtin-baseline') { throw 'vcpkg checkout does not match the declared baseline' }
    $clangPaths = @('clang-cl', 'clang++', 'clang') | ForEach-Object {
        $command = Get-Command $_ -ErrorAction SilentlyContinue
        if ($command) { $command.Source }
    }
    foreach ($candidate in @('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe',
        'C:/Program Files/LLVM/bin/clang-cl.exe'))
    {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $clangPaths += $candidate }
    }
    Write-Json ([ordered]@{ discovered_clang = @($clangPaths); clang_libfuzzer_ubsan = 'NOT RUN';
        reason = 'Final Gate uses the approved MSVC deterministic mutation + ASan configuration; no LLVM installation' }) `
        (Join-Path $evidenceRoot 'compiler-limitations.json')

    $optionCommon = @('-S', $SourceRoot, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DPB_BUILD_APPS=OFF', '-DPB_BUILD_TOOLS=OFF')
    $defaultBuild = Join-Path $SourceRoot "build-p015-$ExpectedCommit-$stamp-default-off"
    $invalidBuild = Join-Path $SourceRoot "build-p015-$ExpectedCommit-$stamp-invalid-option"
    foreach ($build in @($defaultBuild, $invalidBuild))
    {
        if (Test-Path -LiteralPath $build) { throw "Option probe cannot reuse build tree: $build" }
    }
    $null = Invoke-GateCommand 'default-off-configure' $cmake ($optionCommon + @('-B', $defaultBuild, '-DBUILD_TESTING=ON', '-DPB_BUILD_TESTS=ON'))
    $defaultCache = Get-Content -LiteralPath (Join-Path $defaultBuild 'CMakeCache.txt') -Raw
    if ($defaultCache -notmatch '(?m)^PB_BUILD_PHASE0_GATE:BOOL=OFF\r?$') { throw 'Gate is not default OFF' }
    $defaultInventory = Invoke-GateCommand 'default-off-inventory' $ctest @('--test-dir', $defaultBuild, '--build-config', 'Release', '--show-only=json-v1')
    if (@(($defaultInventory.stdout | ConvertFrom-Json).tests | Where-Object { $_.name -like 'PBPhase0*' }).Count -ne 0)
    {
        throw 'Default configuration unexpectedly contains slow Phase 0 tests'
    }
    $invalid = Invoke-GateCommand 'invalid-option-configure' $cmake ($optionCommon + @('-B', $invalidBuild,
        '-DBUILD_TESTING=OFF', '-DPB_BUILD_TESTS=OFF', '-DPB_BUILD_PHASE0_GATE=ON')) -AllowedExitCodes @(1)
    if (($invalid.stdout + $invalid.stderr) -notmatch 'PB_BUILD_PHASE0_GATE requires') { throw 'Invalid option failed for an unrelated reason' }

    $common = @('-S', $SourceRoot, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DBUILD_TESTING=ON',
        '-DPB_BUILD_TESTS=ON', '-DPB_BUILD_APPS=ON', '-DPB_BUILD_TOOLS=ON', '-DPB_BUILD_PHASE0_GATE=ON',
        '-DPB_TREAT_WARNINGS_AS_ERRORS=ON', '-DPB_BUILD_BENCHMARKS=OFF')
    $testResults = [ordered]@{}
    foreach ($flavor in @('release', 'asan'))
    {
        $isAsan = $flavor -eq 'asan'
        $build = if ($isAsan) { $asanBuild } else { $releaseBuild }
        $configuration = if ($isAsan) { 'RelWithDebInfo' } else { 'Release' }
        $fuzz = if ($isAsan) { 'ON' } else { 'OFF' }
        $null = Invoke-GateCommand "$flavor-configure" $cmake ($common + @('-B', $build, "-DPB_BUILD_FUZZERS=$fuzz"))
        $cachePath = Join-Path $build 'CMakeCache.txt'
        $script:identityFiles[$cachePath] = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash
        $null = Invoke-GateCommand "$flavor-default-build" $cmake @('--build', $build, '--config', $configuration, '--parallel', "$Parallel")
        Register-BuildIdentity $build $configuration $isAsan
        $inventory = Invoke-GateCommand "$flavor-inventory" $ctest @('--test-dir', $build, '--build-config', $configuration, '--show-only=json-v1')
        Set-Content -LiteralPath (Join-Path $evidenceRoot "$flavor-inventory.json") -Value $inventory.stdout -Encoding utf8
        $junit = Join-Path $evidenceRoot "$flavor-ctest.xml"
        $null = Invoke-GateCommand "$flavor-all-ctest" $ctest @('--test-dir', $build, '--build-config', $configuration,
            '--output-on-failure', '--parallel', "$Parallel", '--output-junit', $junit) -TimeoutSeconds 7200
        $testResults[$flavor] = Check-JUnit $junit
        $golden = Invoke-GateCommand "$flavor-golden" (Join-Path $build "tests/golden/$configuration/PBGoldenVectorCheck.exe") @((Join-Path $SourceRoot 'tests/golden'))
        if ($golden.stdout -notmatch 'total=30 failures=0') { throw "$flavor Golden count/pins changed" }
        $null = Invoke-GateCommand "$flavor-H01-H03-resume" (Join-Path $build "tests/PBReceiver/$configuration/PBReceiverTests.exe") @('[completion],[resume-completed]', '--rng-seed', '20260827')
        $null = Invoke-GateCommand "$flavor-H02" (Join-Path $build "tests/PBOuterFec/$configuration/PBOuterFecTests.exe") @('[direct-repeat][allocation],[direct-repeat][boundary]', '--rng-seed', '20260827')
        foreach ($path in Get-ChildItem -LiteralPath (Join-Path $build "tests/Phase0Gate/$configuration") -Filter '*.jsonl' -File)
        {
            Copy-Item -LiteralPath $path.FullName -Destination (Join-Path $evidenceRoot "$flavor-$($path.Name)")
        }
    }
    foreach ($label in @('corpus', 'structured', 'parser-harness'))
    {
        $junit = Join-Path $evidenceRoot "asan-$label.xml"
        $null = Invoke-GateCommand "asan-$label" $ctest @('--test-dir', $asanBuild, '--build-config', 'RelWithDebInfo',
            '-L', $label, '--output-on-failure', '--parallel', "$Parallel", '--output-junit', $junit) -TimeoutSeconds 3600
        $testResults[$label] = Check-JUnit $junit
    }
    $mutationRoot = Join-Path $evidenceRoot 'mutation'
    $null = Invoke-GateCommand 'mutation-830000' $powershell @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'InvokeMutationGate.ps1'),
        '-FuzzBinaryDirectory', (Join-Path $asanBuild 'fuzz/RelWithDebInfo'), '-EvidenceDirectory', $mutationRoot) -TimeoutSeconds 7200
    $mutations = @(Get-Content -LiteralPath (Join-Path $mutationRoot 'mutation-results.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    if ($mutations.Count -ne 11 -or ($mutations | Measure-Object -Property iterations -Sum).Sum -ne 830000 -or
        @($mutations | Where-Object { $_.status -ne 'pass' -or -not $_.completion_marker }).Count -ne 0)
    {
        throw 'Mutation budget/normal termination marker incomplete'
    }
    foreach ($round in @('first', 'second'))
    {
        foreach ($category in @('transport', 'interleave', 'ldpc'))
        {
            $null = Invoke-GateCommand "vectors-$round-$category" (Join-Path $releaseBuild 'tools/Release/PBVectorGen.exe') `
                @('write-corpus', (Join-Path $evidenceRoot "vectors-$round"), '--category', $category, '--seed', '12648430')
        }
    }
    Compare-VectorTrees (Join-Path $evidenceRoot 'vectors-first') (Join-Path $evidenceRoot 'vectors-second')
    foreach ($flavor in @('release', 'asan'))
    {
        $build = if ($flavor -eq 'asan') { $asanBuild } else { $releaseBuild }
        $configuration = if ($flavor -eq 'asan') { 'RelWithDebInfo' } else { 'Release' }
        $modes = if ($flavor -eq 'asan') { 'fast,resume' } else { 'fast,resume,large' }
        $replayScript = Join-Path $PSScriptRoot 'InvokeReferenceReplay.ps1'
        $gate = Join-Path $build "tests/Phase0Gate/$configuration/PBPhase0Gate.exe"
        $replayRoot = Join-Path $evidenceRoot "$flavor-replay"
        $null = Invoke-GateCommand "$flavor-reference-two-rounds" $powershell @('-NoProfile', '-File',
            $replayScript, '-GateExecutable', $gate, '-EvidenceDirectory', $replayRoot, '-ModesCsv', $modes) -TimeoutSeconds 7200
    }
    foreach ($mode in @('fast', 'resume'))
    {
        $stable = foreach ($flavor in @('release', 'asan'))
        {
            $lines = Get-Content -LiteralPath (Join-Path $evidenceRoot "$flavor-replay/round-1-$mode.jsonl")
            ,@($lines | ForEach-Object {
                $record = $_ | ConvertFrom-Json
                $record.PSObject.Properties.Remove('elapsed_ms')
                $record | ConvertTo-Json -Depth 10 -Compress
            })
        }
        if (($stable[0] -join "`n") -cne ($stable[1] -join "`n")) { throw "Cross-configuration evidence differs: $mode" }
    }
    Write-Json ([ordered]@{ commit = $ExpectedCommit; status = 'pass'; modes = @('fast', 'resume'); ignored_fields = @('elapsed_ms') }) `
        (Join-Path $evidenceRoot 'cross-configuration-comparison.json')
    $staticResults = foreach ($project in @('libs/PBReceiver/PBReceiver', 'libs/PBOuterFec/PBOuterFec', 'libs/PBProtocol/PBProtocol',
        'tests/Phase0Gate/PBPhase0Gate', 'tests/PBReceiver/PBReceiverTests', 'tests/PBOuterFec/PBOuterFecTests', 'tests/PBProtocol/PBProtocolTests',
        'fuzz/PBModulationReferenceRasterFuzz'))
    {
        $build = if ($project.StartsWith('fuzz/')) { $asanBuild } else { $releaseBuild }
        $name = ($project -split '/')[-1]
        $result = Invoke-GateCommand "cppcheck-$name" $CppcheckExecutable @("--project=$(Join-Path $build ($project + '.vcxproj'))",
            '--language=c++', '--std=c++20', '--platform=win64', '--check-level=exhaustive', '--inconclusive',
            '--enable=warning,style,performance,portability', '--suppress=missingIncludeSystem', '--error-exitcode=2') -AllowedExitCodes @(0, 2)
        [ordered]@{ project = $project; exit_code = $result.exit_code; stdout = $result.prefix + '.stdout.log';
            stderr = $result.prefix + '.stderr.log'; classification = 'Requires human review; exit 2 is not an automatic pass' }
    }
    Write-Json @($staticResults) (Join-Path $evidenceRoot 'cppcheck-results.json')
    $artifactHashes = foreach ($entry in $script:identityFiles.GetEnumerator() | Sort-Object Key)
    {
        [ordered]@{ path = $entry.Key; sha256 = $entry.Value }
    }
    Write-Json @($artifactHashes) (Join-Path $evidenceRoot 'tested-artifacts-sha256.json')
    Assert-Identity 'execution-seal' 'final'
    Write-Json ([ordered]@{ commit = $ExpectedCommit; execution_status = 'pass'; worktree = 'clean'; tests = $testResults;
        mutation_drivers = 11; mutation_iterations = 830000; reference_boundary_mutations = 128;
        vector_seed = 12648430; reference_replay_rounds = 2; FinalSystemComplete = 'NO';
        tag_created_by_driver = $false; decision = 'Pending final code/static review and evidence seal; this is not tag authorization' }) `
        (Join-Path $evidenceRoot 'execution-summary.json')
    Write-Host "FINAL_GATE_EXECUTION_COMPLETED commit=$ExpectedCommit evidence=$evidenceRoot"
}
catch
{
    if (Test-Path -LiteralPath $evidenceRoot)
    {
        Write-Json ([ordered]@{ commit = $ExpectedCommit; execution_status = 'fail'; stage = $script:lastStage;
            error = $_.Exception.Message; Phase0Acceptance = 'FAIL'; FinalSystemComplete = 'NO'; tag_created = $false }) `
            (Join-Path $evidenceRoot 'execution-summary.json')
    }
    throw
}

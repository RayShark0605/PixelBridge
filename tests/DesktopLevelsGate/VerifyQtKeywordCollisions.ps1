#Requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$AppsRoot,
    [Parameter(Mandatory)][string]$GateScript,
    [Parameter(Mandatory)][string]$ScratchRoot,
    [Parameter(Mandatory)][string]$HeaderClosureRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# InvokeFinalGate.ps1 runs cppcheck on every apps/* project with the Qt keyword shims
# '-Dslots=' '-Dsignals=public' '-DQ_OBJECT=' '-Demit=' because cppcheck has no moc support.
# A -D shim rewrites the *identifier*, so a member, variable or parameter that happens to be
# named slots/signals/emit is deleted during preprocessing: 'state.slots.resize(n)' becomes
# 'state..resize(n)'. cppcheck then reports a syntax error and stops analysing that translation
# unit, so static-review coverage for the file silently drops to zero while --error-exitcode=2
# only shows an unrelated syntax error.
# Qt's own label/keyword forms ('slots:', 'signals:', 'emit Call();') are the only legal uses;
# anything else is rejected here, before the Gate can lose coverage. Private libs/* sources are
# analysed without the shims, but public libs/*/include headers are scanned because apps translation
# units can include them while the Qt shims are active.

$expectedShims = "'-Dslots=','-Dsignals=public','-DQ_OBJECT=','-Demit=')"
# Comments and literals are skipped first: prose about capture slots must not trip the guard.
$tokenPattern = "(?s)/\*.*?\*/|//[^\n]*|`"(?:\\.|[^`"\\\n])*`"|'(?:\\.|[^'\\\n])*'|\b(?:slots|signals|emit)\b"

function Get-QtKeywordViolations
{
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $violations = [System.Collections.Generic.List[string]]::new()
    foreach ($token in [regex]::Matches($Text, $tokenPattern))
    {
        if ($token.Value.StartsWith('/') -or $token.Value.StartsWith('"') -or $token.Value.StartsWith("'"))
        {
            continue
        }
        $before = $Text.Substring(0, $token.Index)
        $trimmedBefore = $before.TrimEnd()
        $trimmedAfter = $Text.Substring($token.Index + $token.Length).TrimStart()
        $usedAsMember = $trimmedBefore.EndsWith('.') -or $trimmedBefore.EndsWith('->') -or $trimmedBefore.EndsWith('::')
        $violation = $null
        if ($token.Value -eq 'slots' -or $token.Value -eq 'signals')
        {
            # Legal form: an access specifier label such as 'signals:' or 'private slots:'.
            if (-not $trimmedAfter.StartsWith(':') -or $usedAsMember) { $violation = 'access-specifier keyword used outside a label' }
        }
        elseif ($usedAsMember -or -not [regex]::IsMatch($trimmedAfter, '^[A-Za-z_~][A-Za-z0-9_]*\s*\('))
        {
            # 'emit' expands to nothing, so it may only sit in front of the emitted call.
            $violation = 'emit keyword is not in front of a signal emission call'
        }
        else
        {
            # Anything else that could bind 'emit' as a name, e.g. 'auto emit = ...'.
            if (-not [regex]::IsMatch($trimmedBefore, '(^|[{;:)]|else|=>)$')) { $violation = 'emit keyword is not at statement start' }
        }
        if ($null -ne $violation)
        {
            $line = 1 + ([regex]::Matches($before, "`n")).Count
            $violations.Add("line $line : $($token.Value) : $violation")
        }
    }
    return $violations
}

function Get-QtKeywordFileScan
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [string]$SubPathRegex = '')
    $violations = [System.Collections.Generic.List[string]]::new()
    $scanned = 0
    foreach ($file in @(Get-ChildItem -Path $Root -Recurse -File -Include '*.cpp', '*.h', '*.hpp', '*.cc', '*.hh', '*.inl' -ErrorAction SilentlyContinue))
    {
        if ($file.FullName -match '[/\\]_autogen[/\\]') { continue }
        if ($SubPathRegex -ne '' -and $file.FullName -notmatch $SubPathRegex) { continue }
        $scanned++
        foreach ($violation in @(Get-QtKeywordViolations -Text ([IO.File]::ReadAllText($file.FullName))))
        {
            $violations.Add("$($file.FullName): $violation")
        }
    }
    return @{ Scanned = $scanned; Violations = $violations }
}

foreach ($accepted in @('class C { public: signals: void Changed(); private slots: void Tick(); };',
                        'void C::Tick() { emit SnapshotChanged(); }',
                        'void C::Other() {
    emit
        TerminalStateReached();
}',
                        'void C::Guard() { if (ready) emit SnapshotChanged(); else emit TerminalStateReached(); }',
                        '// reserve a fixed number of preallocated slots for capture
std::cout << "senders emit exact code values" << "signals"; /* slots and emit too */'))
{
    $acceptedViolations = @(Get-QtKeywordViolations -Text $accepted)
    if ($acceptedViolations.Count -ne 0)
    {
        throw "Legal Qt keyword usage was rejected: $($acceptedViolations[0]) in [$accepted]"
    }
}

$rejectedCases = [ordered]@{
    'struct S { std::vector<int> slots; };'                     = 'slots member'
    'void F() { state.slots.resize(4); }'                       = 'slots member access'
    'int signals = 0;'                                          = 'signals variable'
    'void F() { state->signals++; }'                            = 'signals member access'
    'void F() { int emit = 0; }'                                = 'emit variable'
    'void F() { state.emit(); }'                                = 'emit member access'
    'void F() { emit counter; }'                                = 'emit on a non-call'
    'void G() { auto slots = CaptureSlots(); }'                 = 'slots local variable'
}
foreach ($rejected in $rejectedCases.Keys)
{
    if (@(Get-QtKeywordViolations -Text $rejected).Count -ne 1)
    {
        throw "Colliding Qt identifier ('$($rejectedCases[$rejected])') was not detected exactly once: '$rejected'"
    }
}

# The file walk itself must work, otherwise the guard passes by scanning nothing.
if (-not (Test-Path -LiteralPath $AppsRoot -PathType Container)) { throw "apps source root not found: $AppsRoot" }
if (-not (Test-Path -LiteralPath $GateScript -PathType Leaf)) { throw "Gate script not found: $GateScript" }
$probe = Join-Path $ScratchRoot ("qt-keyword-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory((Join-Path $probe 'sub')) | Out-Null
[IO.File]::WriteAllText((Join-Path $probe 'bad.cpp'), 'void Recorder::Open() { state.slots.resize(4); emit Ready(); }')
[IO.File]::WriteAllText((Join-Path $probe 'sub\good.h'), 'class C { public: signals: void Ready(); private slots: void Tick(); };')
$probeScan = Get-QtKeywordFileScan -Root $probe
if ($probeScan.Scanned -ne 2 -or $probeScan.Violations.Count -ne 1)
{
    throw "Qt keyword file scan is broken: files=$($probeScan.Scanned) violations=$($probeScan.Violations.Count)"
}

# The cppcheck -D shims also rewrite every header an apps translation unit includes, so the public
# header closure of libs/* is part of the guarded surface even though libs projects themselves are
# analysed without shims. A 'slots' member in a public header would therefore delete code from an
# apps translation unit and silently drop it from static review.
$closureProbe = Join-Path $ScratchRoot ("qt-closure-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory((Join-Path $closureProbe 'pbx\include\pbx')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $closureProbe 'pbx\src')) | Out-Null
[IO.File]::WriteAllText((Join-Path $closureProbe 'pbx\include\pbx\public.h'), 'class PublicHeader { std::vector<int> slots; };')
[IO.File]::WriteAllText((Join-Path $closureProbe 'pbx\src\internal.h'), 'class PrivateHeader { std::vector<int> signals; };')
$closureProbeScan = Get-QtKeywordFileScan -Root $closureProbe -SubPathRegex '[/\\]include[/\\]'
if ($closureProbeScan.Scanned -ne 1 -or $closureProbeScan.Violations.Count -ne 1)
{
    throw "Qt keyword header-closure scan is broken: files=$($closureProbeScan.Scanned) violations=$($closureProbeScan.Violations.Count)"
}

# The real closure must be scanned too, and scanning nothing must never look like success.
if (-not (Test-Path -LiteralPath $HeaderClosureRoot -PathType Container)) { throw "header closure root not found: $HeaderClosureRoot" }
$headerScan = Get-QtKeywordFileScan -Root $HeaderClosureRoot -SubPathRegex '[/\\]include[/\\]'
if ($headerScan.Scanned -eq 0) { throw "Qt keyword guard scanned no public headers below $HeaderClosureRoot" }
if ($headerScan.Violations.Count -ne 0)
{
    $headerDetail = $headerScan.Violations -join "`n"
    throw "public headers below $HeaderClosureRoot use a cppcheck-shimmed Qt keyword as an identifier, so an apps translation unit would silently leave static review:`n$headerDetail"
}

$appsScan = Get-QtKeywordFileScan -Root $AppsRoot
if ($appsScan.Scanned -eq 0) { throw "Qt keyword guard scanned no apps sources below $AppsRoot" }
if ($appsScan.Violations.Count -ne 0)
{
    $detail = $appsScan.Violations -join "`n"
    throw "apps sources use a cppcheck-shimmed Qt keyword as an identifier, so the static-review Gate would silently lose that translation unit:`n$detail"
}

# Keep the guard in sync with the Gate: an extra or removed shim changes which identifiers the
# preprocessor destroys, so the guard above has to be re-verified deliberately.
$gateText = [IO.File]::ReadAllText($GateScript)
if ([regex]::Matches($gateText, [regex]::Escape($expectedShims)).Count -ne 1)
{
    throw 'Qt keyword shim list in InvokeFinalGate.ps1 changed, or is applied more than once; re-verify the identifier guard and update this check'
}
# The shims only work around the absent moc in apps/*. If they ever reach the libs/* or tools/*
# projects, a real 'slots'/'signals'/'emit' identifier there is no longer covered by the scan above,
# so such a translation unit could leave static review with only an unrelated cppcheck symptom.
$shimIndex = $gateText.IndexOf($expectedShims, [StringComparison]::Ordinal)
if ($shimIndex -lt 0)
{
    throw 'Qt keyword shim list is missing from InvokeFinalGate.ps1; re-verify the identifier guard and update this check'
}
$appGuard = @([regex]::Matches($gateText, "StartsWith\('apps/'\)") | Where-Object { $_.Index -lt $shimIndex })
if ($appGuard.Count -eq 0)
{
    throw 'InvokeFinalGate.ps1 applies the Qt keyword shims without an apps/* test above them; extend this guard to every shimmed source root and update this check'
}
$enclosingGuard = $appGuard[-1]
$textBetweenGuardAndShims = [regex]::Replace($gateText.Substring($enclosingGuard.Index, $shimIndex - $enclosingGuard.Index), '//[^\r\n]*', '')
if ($textBetweenGuardAndShims -match '(?m)^\s*\}')
{
    throw 'Qt keyword shims in InvokeFinalGate.ps1 escaped the apps/* branch; extend this guard to every shimmed source root and update this check'
}

Write-Output "QT_KEYWORD_COLLISION_PASS apps-files=$($appsScan.Scanned) header-files=$($headerScan.Scanned) violations=$($appsScan.Violations.Count + $headerScan.Violations.Count) accepted-cases=5 rejected-cases=$($rejectedCases.Count) file-scan-self-test=1 closure-self-test=1 shim-list=apps-only"

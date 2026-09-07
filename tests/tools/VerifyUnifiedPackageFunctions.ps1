#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$VerifierPath, [Parameter(Mandatory = $true)][string]$WorkRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $root) { throw 'Create-only test root already exists' }
[void](New-Item -ItemType Directory -Path $root)
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($VerifierPath), [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count) { throw 'Verifier syntax errors' }
$requiredFunctions = @('Get-StreamSha256', 'Test-RelativePath', 'Assert-StrictJsonElement', 'Require-UnsignedInteger', 'Read-BoundedJson')
foreach ($name in $requiredFunctions) {
    $definitions = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true))
    if ($definitions.Count -ne 1) { throw "Missing/ambiguous verifier function: $name" }
    # Only these checked-in function definitions are loaded; never the main
    # verifier, package input, arbitrary fixture text or packaged executable.
    . ([ScriptBlock]::Create($definitions[0].Extent.Text))
}
$results = [Collections.Generic.List[object]]::new()
function Require-Test([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "Test failed: $Name" }
    $results.Add(@{ name = $Name; passed = $true })
}
function Require-Rejection([scriptblock]$Action, [string]$Name) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require-Test $rejected $Name
}

Require-Test (Test-RelativePath -Path 'Encoder/PixelBridgeEncoder.exe') 'ordinary portable path accepted'
foreach ($path in @('../outside', '/absolute', 'a/../b', 'C:/file', 'a:stream', 'Encoder\x.dll', 'a/NUL.txt', 'a/LPT1', 'x.', 'x ', 'a?b', 'a|b')) {
    Require-Test (-not (Test-RelativePath -Path $path)) "unsafe Windows path rejected: $path"
}
foreach ($value in @(-1, 1.5, '3', $true)) {
    Require-Rejection { Require-UnsignedInteger -Value $value -Name 'size' } "non-integer JSON size rejected: $value"
}
Require-UnsignedInteger -Value 0 -Name 'size'
Require-UnsignedInteger -Value ([UInt64]2147483648) -Name 'size'
Require-Test $true 'unsigned boundary values accepted'
$jsonCases = @(
    @{ name = 'duplicate-key'; text = '{"a":1,"a":2}' },
    @{ name = 'case-alias'; text = '{"a":1,"A":2}' },
    @{ name = 'nested-duplicate'; text = '{"a":[{"x":1,"x":2}]}' },
    @{ name = 'trailing-comma'; text = '{"a":1,}' },
    @{ name = 'comment'; text = '{/*x*/"a":1}' }
)
foreach ($test in $jsonCases) {
    $path = Join-Path $root ($test.name + '.json')
    [IO.File]::WriteAllText($path, $test.text, [Text.UTF8Encoding]::new($false))
    Require-Rejection { Read-BoundedJson -Path $path } "ambiguous JSON rejected: $($test.name)"
}
$validPath = Join-Path $root 'valid.json'
[IO.File]::WriteAllText($validPath, '{"a":[0,1,2]}', [Text.UTF8Encoding]::new($false))
$valid = Read-BoundedJson -Path $validPath
Require-Test ($valid.a.Count -eq 3) 'bounded valid JSON accepted'

$data = [Text.Encoding]::ASCII.GetBytes('abc')
$stream = [IO.MemoryStream]::new($data, $false)
try {
    $hash = Get-StreamSha256 -Stream $stream -ExpectedBytes 3
    Require-Test ($hash -ceq 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') 'bounded stream SHA-256 correct'
} finally { $stream.Dispose() }
foreach ($length in @(0, 2, 4)) {
    $stream = [IO.MemoryStream]::new($data, $false)
    try { Require-Rejection { Get-StreamSha256 -Stream $stream -ExpectedBytes $length } "stream length mismatch rejected: $length" }
    finally { $stream.Dispose() }
}
$summary = @{ schema = 'PixelBridge.G22.PackageVerifierFunctions.1'; passed = $true; cases = $results.Count; results = $results; packagedExecutablesRun = $false }
[IO.File]::WriteAllText((Join-Path $root 'summary.json'), ($summary | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
"PASS $($results.Count) bounded package verifier function checks"

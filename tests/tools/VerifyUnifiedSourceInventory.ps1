#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$EvidenceDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($EvidenceDirectory)
if (Test-Path -LiteralPath $root) { throw 'Evidence directory must not exist' }
[void][IO.Directory]::CreateDirectory($root)
$fixture = Join-Path $root 'source-fixture'
[void][IO.Directory]::CreateDirectory($fixture)
$producer = Join-Path $PSScriptRoot '../../tools/PBUnifiedRelease/New-PBUnifiedPortablePackage.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($producer, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Producer parse failed' }
$definitions = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-GitSourcePaths'
}, $false))
if ($definitions.Count -ne 1) { throw 'Expected one source inventory helper' }
# Load only the trusted, repository-local function, not the release entrypoint.
. ([scriptblock]::Create($definitions[0].Extent.Text))
$encoding = [Text.UTF8Encoding]::new($false)
$trackedNames = @('docs/PixelBridge_最终技术路线与总体设计.md', 'docs/résumé with spaces.md', '.gitignore')
$untrackedName = 'notes/日本語[1].txt'
foreach ($name in ($trackedNames + @($untrackedName, 'ignored.bin'))) {
    $path = Join-Path $fixture $name
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $(if ($name -eq '.gitignore') { '/ignored.bin' } else { 'fixture' }), $encoding)
}
& git -C $fixture init --quiet
if ($LASTEXITCODE -ne 0) { throw 'Fixture Git init failed' }
& git -C $fixture add -- @trackedNames
if ($LASTEXITCODE -ne 0) { throw 'Fixture Git add failed' }
$originalEncoding = [Console]::OutputEncoding
$results = @()
try {
    foreach ($codePage in @(936, 65001)) {
        [Console]::OutputEncoding = [Text.Encoding]::GetEncoding($codePage)
        $paths = @(Get-GitSourcePaths -RepositoryRoot $fixture)
        $expected = $trackedNames + @($untrackedName)
        if ($paths.Count -ne $expected.Count) { throw 'Incorrect inventory count' }
        foreach ($path in $expected) {
            if ($paths -cnotcontains $path -or -not (Test-Path -LiteralPath (Join-Path $fixture $path) -PathType Leaf)) {
                throw "Source path corrupted under code page ${codePage}: $path"
            }
        }
        if ([Console]::OutputEncoding.CodePage -ne $codePage) { throw 'Helper changed console encoding' }
        $results += @{ codePage = $codePage; passed = $true; paths = $paths }
    }
    $rejected = $false
    try { $null = Get-GitSourcePaths -RepositoryRoot (Join-Path $root 'nonexistent') }
    catch { $rejected = $_.Exception.Message -like '*Unable to enumerate source tree*' }
    if (-not $rejected) { throw 'Non-repository input was not rejected' }
}
finally {
    [Console]::OutputEncoding = $originalEncoding
}
$result = @{ passed = $true; cases = $results; missingRepositoryRejected = $rejected; consoleEncodingRestored = $true }
[IO.File]::WriteAllText((Join-Path $root 'result.json'), ($result | ConvertTo-Json -Depth 8), $encoding)
$result | ConvertTo-Json -Depth 8

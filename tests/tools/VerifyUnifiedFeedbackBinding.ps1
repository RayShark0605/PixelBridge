#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$VerifierPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($VerifierPath), [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count) { throw 'Verifier syntax errors' }
$definitions = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-FeedbackHelperBinding'
}, $false))
if ($definitions.Count -ne 1) { throw 'Missing feedback-helper binding validator' }
. ([scriptblock]::Create($definitions[0].Extent.Text))

function New-Fixture
{
    $hash = 'a' * 64
    return @{
        application = @{
            relativeExecutablePath = 'PixelBridgeEncoder.exe'
            runtimeIdentity = @{ applicationVersion = '1.1.0' }
            feedbackHelper = @{
                relativeExecutablePath = 'PBFeedbackIo.exe'; size = [UInt64]100; sha256 = $hash
                runtimeIdentity = @{ application = 'PBFeedbackIo'; feedbackProtocol = 1; ipcProtocol = 1; metadataOnly = $true }
            }
        }
        files = @{ 'PBFeedbackIo.exe' = @{ size = [UInt64]100; sha256 = $hash } }
    }
}

$cases = 0
function Require-Rejection([scriptblock]$Change, [string]$Name)
{
    $fixture = New-Fixture
    & $Change $fixture
    $rejected = $false
    try { Assert-FeedbackHelperBinding -Application $fixture.application -FileMap $fixture.files }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Accepted invalid helper: $Name" }
    $script:cases++
}

$fixture = New-Fixture
$fileMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
$fileMap.Add('PBFeedbackIo.exe', $fixture.files['PBFeedbackIo.exe'])
Assert-FeedbackHelperBinding -Application $fixture.application -FileMap $fileMap
$cases++
$legacy = @{ runtimeIdentity = @{ applicationVersion = '1.0.0' }; relativeExecutablePath = 'PixelBridgeEncoder.exe' }
Assert-FeedbackHelperBinding -Application $legacy -FileMap @{}
$cases++
Require-Rejection { param($value) $value.application.Remove('feedbackHelper') } 'missing binding'
Require-Rejection { param($value) $value.files.Clear() } 'missing helper after manifest rebinding'
Require-Rejection { param($value) $value.application.feedbackHelper.sha256 = 'b' * 64 } 'different binary'
Require-Rejection { param($value) $value.application.feedbackHelper.size = '100' } 'string size'
Require-Rejection { param($value) $value.application.feedbackHelper.relativeExecutablePath = 'elsewhere/PBFeedbackIo.exe' } 'not a sibling'
Require-Rejection { param($value) $value.application.feedbackHelper.runtimeIdentity.metadataOnly = $false } 'payload boundary changed'
Require-Rejection { param($value) $value.application.feedbackHelper.runtimeIdentity.metadataOnly = 'true' } 'string boolean'
Require-Rejection { param($value) $value.application.feedbackHelper.runtimeIdentity.feedbackProtocol = 2 } 'wrong feedback protocol'
Require-Rejection { param($value) $value.application.feedbackHelper.runtimeIdentity.ipcProtocol = 2 } 'wrong IPC protocol'
Require-Rejection { param($value) $value.application.feedbackHelper.runtimeIdentity.extra = 1 } 'unexpected capability'
if ($cases -ne 12) { throw 'Unexpected test count' }
Write-Output "PASS: $cases feedback-helper binding cases; no packaged executable was run."

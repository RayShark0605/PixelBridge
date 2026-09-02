[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolsRoot,

    [Parameter(Mandatory = $true)]
    [string]$WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-ScriptFunction
{
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0)
    {
        throw "PowerShell parser rejected $ScriptPath`: $($parseErrors[0].Message)"
    }
    $definitions = @($ast.FindAll({
                param($node)
                $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $FunctionName
            }, $true))
    if ($definitions.Count -ne 1)
    {
        throw "Expected exactly one $FunctionName definition in $ScriptPath"
    }
    $scriptScopedDefinition = $definitions[0].Extent.Text -replace
        ("^function\s+" + [regex]::Escape($FunctionName)), ("function script:" + $FunctionName)
    . ([ScriptBlock]::Create($scriptScopedDefinition))
}

function Write-Journal
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object[]]$Records
    )

    $content = (@($Records | ForEach-Object { $_ | ConvertTo-Json -Compress }) -join [Environment]::NewLine) +
        [Environment]::NewLine
    [IO.File]::WriteAllText($Path, $content, [Text.UTF8Encoding]::new($false))
}

function Require-Failure
{
    param(
        [Parameter(Mandatory = $true)][ScriptBlock]$Action,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $failed = $false
    try
    {
        & $Action
    }
    catch
    {
        $failed = $true
    }
    if (-not $failed)
    {
        throw "$Name unexpectedly succeeded"
    }
}

function New-Record
{
    param(
        [Parameter(Mandatory = $true)][Int64]$UnixMilliseconds,
        [Parameter(Mandatory = $true)][bool]$DescriptorKnown,
        [Parameter(Mandatory = $true)][UInt64]$ResourceRejections,
        [UInt64]$ConflictRejections = 0,
        [string]$State = 'WaitingForBootstrap',
        [bool]$DigestPass = $false,
        [bool]$PublishPass = $false,
        [string]$RunId = '0123456789abcdef0123456789abcdef'
    )

    return [ordered]@{
        schema = 'PixelBridge.RunJournal.1'
        role = 'Decoder'
        unixMs = $UnixMilliseconds
        runId = $RunId
        state = $State
        descriptorKnown = $DescriptorKnown
        outerResourceRejections = $ResourceRejections
        outerConflictRejections = $ConflictRejections
        wholeFileDigestPass = $DigestPass
        finalPublishPass = $PublishPass
    }
}

$resolvedToolsRoot = [IO.Path]::GetFullPath($ToolsRoot)
$resolvedWorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if (-not (Test-Path -LiteralPath $resolvedToolsRoot -PathType Container))
{
    throw "Tool root does not exist: $resolvedToolsRoot"
}
[IO.Directory]::CreateDirectory($resolvedWorkRoot) | Out-Null
$runRoot = Join-Path $resolvedWorkRoot ("run-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
try
{
    $producerPath = Join-Path $resolvedToolsRoot 'Invoke-PBLocalDesktopRegressionGate.ps1'
    $verifierPath = Join-Path $resolvedToolsRoot 'Test-PBLocalDesktopRegressionEvidence.ps1'
    foreach ($path in @($producerPath, $verifierPath))
    {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf))
        {
            throw "Required Step 19 script is missing: $path"
        }
        $text = Get-Content -Raw -LiteralPath $path
        if ($text -match '\bInvoke-Expression\b' -or $text -match '\bRemove-Item\b')
        {
            throw "Step 19 script contains a forbidden dynamic-execution or deletion primitive: $path"
        }
    }

    $producerText = Get-Content -Raw -LiteralPath $producerPath
    $decoderStartIndex = $producerText.IndexOf('$decoderProcess = Start-Process', [StringComparison]::Ordinal)
    $encoderStartIndex = $producerText.IndexOf('$encoderProcess = Start-Process', [StringComparison]::Ordinal)
    if ($decoderStartIndex -lt 0 -or $encoderStartIndex -le $decoderStartIndex -or
        $producerText -notmatch 'postDescriptorResourceRejectionIncrease -eq 0' -or
        $producerText -notmatch 'FileMode\]::CreateNew')
    {
        throw 'Producer launch alignment, post-descriptor guard, or create-only contract is missing'
    }

    Import-ScriptFunction -ScriptPath $producerPath -FunctionName 'Get-DecoderJournalAnalysis'
    Import-ScriptFunction -ScriptPath $verifierPath -FunctionName 'Get-VerifiedDecoderJournal'
    $runId = '0123456789abcdef0123456789abcdef'
    $validRecords = @(
        New-Record -UnixMilliseconds 1000 -DescriptorKnown $false -ResourceRejections 0
        New-Record -UnixMilliseconds 2000 -DescriptorKnown $false -ResourceRejections 7
        New-Record -UnixMilliseconds 3000 -DescriptorKnown $true -ResourceRejections 7 -State 'Recovering'
        New-Record -UnixMilliseconds 4000 -DescriptorKnown $true -ResourceRejections 7 -State 'Completed' -DigestPass $true -PublishPass $true
    )
    $validPath = Join-Path $runRoot 'valid.jsonl'
    Write-Journal -Path $validPath -Records $validRecords
    $producerAnalysis = Get-DecoderJournalAnalysis -Path $validPath -RunId $runId
    $verifierAnalysis = Get-VerifiedDecoderJournal -Path $validPath -RunId $runId
    foreach ($analysis in @($producerAnalysis, $verifierAnalysis))
    {
        if ([int]$analysis.recordCount -ne 4 -or [int]$analysis.preDescriptorRecordCount -ne 2 -or
            [UInt64]$analysis.resourceRejectionsAtFirstDescriptorSample -ne 7 -or
            [UInt64]$analysis.finalResourceRejections -ne 7 -or
            [UInt64]$analysis.postDescriptorResourceRejectionIncrease -ne 0 -or
            $analysis.terminalState -cne 'Completed' -or -not [bool]$analysis.terminalWholeFileDigestPass -or
            -not [bool]$analysis.terminalFinalPublishPass)
        {
            throw 'Valid journal analysis returned the wrong authoritative summary'
        }
    }

    $postDescriptorIncrease = @(
        New-Record -UnixMilliseconds 1000 -DescriptorKnown $false -ResourceRejections 0
        New-Record -UnixMilliseconds 2000 -DescriptorKnown $false -ResourceRejections 7
        New-Record -UnixMilliseconds 3000 -DescriptorKnown $true -ResourceRejections 7 -State 'Recovering'
        New-Record -UnixMilliseconds 4000 -DescriptorKnown $true -ResourceRejections 8 -State 'Completed' -DigestPass $true -PublishPass $true
    )
    $increasePath = Join-Path $runRoot 'post-descriptor-increase.jsonl'
    Write-Journal -Path $increasePath -Records $postDescriptorIncrease
    Require-Failure -Name 'producer post-descriptor increase guard' -Action {
        Get-DecoderJournalAnalysis -Path $increasePath -RunId $runId | Out-Null
    }
    $independentIncrease = Get-VerifiedDecoderJournal -Path $increasePath -RunId $runId
    if ([UInt64]$independentIncrease.postDescriptorResourceRejectionIncrease -ne 1)
    {
        throw 'Independent verifier did not expose the post-descriptor resource increase'
    }

    $conflictRecords = @(
        New-Record -UnixMilliseconds 1000 -DescriptorKnown $false -ResourceRejections 0
        New-Record -UnixMilliseconds 2000 -DescriptorKnown $false -ResourceRejections 7
        New-Record -UnixMilliseconds 3000 -DescriptorKnown $true -ResourceRejections 7 -ConflictRejections 1 -State 'Recovering'
        New-Record -UnixMilliseconds 4000 -DescriptorKnown $true -ResourceRejections 7 -ConflictRejections 1 -State 'Completed' -DigestPass $true -PublishPass $true
    )
    $conflictPath = Join-Path $runRoot 'conflict.jsonl'
    Write-Journal -Path $conflictPath -Records $conflictRecords
    Require-Failure -Name 'producer conflict guard' -Action {
        Get-DecoderJournalAnalysis -Path $conflictPath -RunId $runId | Out-Null
    }
    Require-Failure -Name 'verifier conflict guard' -Action {
        Get-VerifiedDecoderJournal -Path $conflictPath -RunId $runId | Out-Null
    }

    $wrongRunPath = Join-Path $runRoot 'wrong-run.jsonl'
    Write-Journal -Path $wrongRunPath -Records @(
        New-Record -UnixMilliseconds 1000 -DescriptorKnown $true -ResourceRejections 0 -RunId ('f' * 32)
        New-Record -UnixMilliseconds 2000 -DescriptorKnown $true -ResourceRejections 0 -State 'Completed' -DigestPass $true -PublishPass $true -RunId ('f' * 32)
    )
    Require-Failure -Name 'producer RunId guard' -Action {
        Get-DecoderJournalAnalysis -Path $wrongRunPath -RunId $runId | Out-Null
    }
    Require-Failure -Name 'verifier RunId guard' -Action {
        Get-VerifiedDecoderJournal -Path $wrongRunPath -RunId $runId | Out-Null
    }

    Write-Output 'PBLocalDesktop regression-evidence contracts: PASS'
}
finally
{
    $resolvedRunRoot = [IO.Path]::GetFullPath($runRoot)
    $workPrefix = $resolvedWorkRoot.TrimEnd('\') + '\'
    if ($resolvedRunRoot.StartsWith($workPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedRunRoot -PathType Container))
    {
        [IO.Directory]::Delete($resolvedRunRoot, $true)
    }
}

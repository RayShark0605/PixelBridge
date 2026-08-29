#Requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'NativeLoopPolicy.ps1')

foreach ($mode in @('release','asan','RELEASE','ASAN'))
{
    $policy = Get-DesktopLevelsNativeLoopPolicy $mode
    $minimumLifetime = [UInt64]($policy.ReceiverDeadlineSeconds + 2) * 1000
    if ($policy.CaptureSeconds -ne 30 -or $policy.SenderLifetimeMilliseconds -le $minimumLifetime -or
        $policy.SenderFrames -gt 1000000 -or $policy.SequenceIntervalMilliseconds -lt 50 -or
        $policy.SequenceIntervalMilliseconds -gt 60000 -or $policy.SenderShutdownMilliseconds -le 0 -or
        $policy.SenderShutdownMilliseconds -gt 60000)
    {
        throw "Invalid bounded native-loop policy for $mode"
    }
    $expectedEvidence = if ($policy.NativeMode -eq 'asan') { 8 } else { 16 }
    if ($policy.RequiredVerifiedFrames -ne $expectedEvidence -or $policy.RequiredVerifiedPhases -ne $expectedEvidence)
    {
        throw "Invalid evidence demand for $mode"
    }
}

$invalidRejected = $false
try
{
    $null = Get-DesktopLevelsNativeLoopPolicy 'unknown'
}
catch
{
    $invalidRejected = $true
}
if (-not $invalidRejected)
{
    throw 'Unknown native Gate mode was accepted'
}

$nativeLoopPath = Join-Path $PSScriptRoot 'InvokeNativeLoop.ps1'
$tokens = $null
$parseErrors = $null
$nativeLoopAst = [Management.Automation.Language.Parser]::ParseFile($nativeLoopPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0)
{
    throw 'InvokeNativeLoop.ps1 does not parse'
}
$parameterNames = @($nativeLoopAst.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath.ToLowerInvariant() })
$parameterAssignments = @($nativeLoopAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst]
}, $true) | Where-Object { $parameterNames -contains $_.Left.VariablePath.UserPath.ToLowerInvariant() })
if ($parameterAssignments.Count -ne 0)
{
    $collision = $parameterAssignments[0]
    throw "Native-loop parameter is overwritten at line $($collision.Extent.StartLineNumber): $($collision.Left.Extent.Text)"
}

Write-Output 'DESKTOP_LEVELS_NATIVE_LOOP_POLICY_PASS modes=4 invalid=1 sender-lifetime-bounded=1 parameter-collisions=0'

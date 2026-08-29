#Requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'Phase1FileGatePolicy.ps1')
. (Join-Path $PSScriptRoot 'Phase1FileResources.ps1')

function New-Sample([Int64]$ElapsedMilliseconds, [Int64]$PrivateDelta = 0, [Int64]$HandleDelta = 0)
{
    return [pscustomobject]@{
        ElapsedMilliseconds=$ElapsedMilliseconds
        SenderPrivateBytes=[Int64](100 * 1024 * 1024 + $PrivateDelta)
        SenderWorkingSetBytes=[Int64](80 * 1024 * 1024 + $PrivateDelta)
        SenderHandles=[Int64](100 + $HandleDelta)
        SenderCpuMilliseconds=[double]$ElapsedMilliseconds * 0.5 + 1
        ReceiverPrivateBytes=[Int64](200 * 1024 * 1024 + $PrivateDelta)
        ReceiverWorkingSetBytes=[Int64](160 * 1024 * 1024 + $PrivateDelta)
        ReceiverHandles=[Int64](200 + $HandleDelta)
        ReceiverCpuMilliseconds=[double]$ElapsedMilliseconds * 0.25 + 1
    }
}

$regularPolicy = Get-Phase1FileGatePolicy 0 'release'
$regular = @((New-Sample 0),(New-Sample 1000),(New-Sample 2000))
$regularSummary = Get-Phase1FileResourceSummary $regular $null $regularPolicy
if ($regularSummary.Samples -ne 3 -or $regularSummary.LongSoakVerified -or
    [Math]::Abs([double]$regularSummary.SenderEquivalentCpuCores - 0.5) -gt 0.000001 -or
    [Math]::Abs([double]$regularSummary.ReceiverEquivalentCpuCores - 0.25) -gt 0.000001)
{
    throw 'Regular resource/CPU summary is incorrect'
}

$soakPolicy = Get-Phase1FileGatePolicy 300 'release'
$soakSamples = [Collections.Generic.List[object]]::new()
for ($second = 0; $second -le 305; $second++)
{
    $late = $second -ge 270
    $soakSamples.Add((New-Sample ([Int64]$second * 1000) $(if ($late) { 1024 * 1024 } else { 0 }) $(if ($late) { 1 } else { 0 })))
}
$published = [Int64]0
$soakSummary = Get-Phase1FileResourceSummary @($soakSamples) $published $soakPolicy
if (-not $soakSummary.LongSoakVerified -or $soakSummary.SenderPrivateMedianDriftBytes -ne 1048576 -or
    $soakSummary.ReceiverPrivateMedianDriftBytes -ne 1048576 -or $soakSummary.SenderHandleMedianDrift -ne 1 -or
    $soakSummary.ReceiverHandleMedianDrift -ne 1 -or $soakSummary.SenderPrivateHighWaterIncreaseBytes -ne 1048576 -or
    $soakSummary.ReceiverPrivateHighWaterIncreaseBytes -ne 1048576 -or
    $soakSummary.SenderHandleHighWaterIncrease -ne 1 -or $soakSummary.ReceiverHandleHighWaterIncrease -ne 1)
{
    throw 'Long-soak baseline/final/high-water windows are incorrect'
}

$failures = 0
foreach ($case in @('missing-publication','late-publication','short-window','zero-cpu','time-regression','cpu-regression','negative-resource'))
{
    try
    {
        if ($case -eq 'missing-publication')
        {
            $null = Get-Phase1FileResourceSummary @($soakSamples) $null $soakPolicy
        }
        elseif ($case -eq 'late-publication')
        {
            $null = Get-Phase1FileResourceSummary @($soakSamples) ([Int64]400000) $soakPolicy
        }
        elseif ($case -eq 'short-window')
        {
            $null = Get-Phase1FileResourceSummary @($soakSamples | Select-Object -First 100) $published $soakPolicy
        }
        elseif ($case -eq 'zero-cpu')
        {
            $zeroCpu = @((New-Sample 0),(New-Sample 1000))
            $zeroCpu[1].SenderCpuMilliseconds = $zeroCpu[0].SenderCpuMilliseconds
            $null = Get-Phase1FileResourceSummary $zeroCpu $null $regularPolicy
        }
        elseif ($case -eq 'time-regression')
        {
            $null = Get-Phase1FileResourceSummary @((New-Sample 1000),(New-Sample 999)) $null $regularPolicy
        }
        elseif ($case -eq 'cpu-regression')
        {
            $cpuRegression = @((New-Sample 0),(New-Sample 1000),(New-Sample 2000))
            $cpuRegression[1].SenderCpuMilliseconds = 2000
            $null = Get-Phase1FileResourceSummary $cpuRegression $null $regularPolicy
        }
        else
        {
            $negativeResource = @((New-Sample 0),(New-Sample 1000))
            $negativeResource[1].ReceiverHandles = -1
            $null = Get-Phase1FileResourceSummary $negativeResource $null $regularPolicy
        }
    }
    catch
    {
        $failures++
    }
}
if ($failures -ne 7)
{
    throw 'Incomplete or invalid resource evidence was accepted'
}

Write-Output 'PHASE1_FILE_RESOURCES_PASS regular=1 soak=1 invalid=7'

#Requires -Version 7.2

function Get-Phase1MedianInt64([object[]]$Values)
{
    if ($Values.Count -eq 0)
    {
        throw 'Cannot calculate a median from an empty resource window'
    }
    $sorted = @($Values | ForEach-Object { [Int64]$_ } | Sort-Object)
    return [Int64]$sorted[[Math]::Floor($sorted.Count / 2)]
}

function Get-Phase1FileResourceSummary(
    [object[]]$Samples,
    [Nullable[Int64]]$PublishedMilliseconds,
    [object]$Policy)
{
    if ($Samples.Count -lt 2)
    {
        throw 'At least two paired process resource samples are required'
    }
    $previous = $null
    foreach ($sample in $Samples)
    {
        $elapsed = [Int64]$sample.ElapsedMilliseconds
        foreach ($property in @('SenderPrivateBytes','SenderWorkingSetBytes','SenderHandles',
            'ReceiverPrivateBytes','ReceiverWorkingSetBytes','ReceiverHandles'))
        {
            if ([Int64]$sample.$property -lt 0)
            {
                throw "Resource sample contains a negative $property"
            }
        }
        foreach ($property in @('SenderCpuMilliseconds','ReceiverCpuMilliseconds'))
        {
            $cpu = [double]$sample.$property
            if ([double]::IsNaN($cpu) -or [double]::IsInfinity($cpu) -or $cpu -lt 0)
            {
                throw "Resource sample contains invalid $property"
            }
        }
        if ($null -ne $previous -and ($elapsed -le [Int64]$previous.ElapsedMilliseconds -or
            [double]$sample.SenderCpuMilliseconds -lt [double]$previous.SenderCpuMilliseconds -or
            [double]$sample.ReceiverCpuMilliseconds -lt [double]$previous.ReceiverCpuMilliseconds))
        {
            throw 'Resource sample chronology or cumulative CPU time regressed'
        }
        $previous = $sample
    }
    $first = $Samples[0]
    $last = $Samples[-1]
    $elapsedMilliseconds = [double]$last.ElapsedMilliseconds - [double]$first.ElapsedMilliseconds
    if ($elapsedMilliseconds -le 0)
    {
        throw 'Resource sample time did not advance'
    }
    $senderCpu = [double]$last.SenderCpuMilliseconds - [double]$first.SenderCpuMilliseconds
    $receiverCpu = [double]$last.ReceiverCpuMilliseconds - [double]$first.ReceiverCpuMilliseconds
    if ($senderCpu -le 0 -or $receiverCpu -le 0)
    {
        throw 'Sender or Receiver CPU time did not advance during the physical transfer'
    }
    $summary = [ordered]@{
        Samples=[UInt64]$Samples.Count
        LogicalProcessorCount=[UInt32][Environment]::ProcessorCount
        SenderEquivalentCpuCores=$senderCpu / $elapsedMilliseconds
        ReceiverEquivalentCpuCores=$receiverCpu / $elapsedMilliseconds
        SenderPrivateHighWaterBytes=[Int64](($Samples | Measure-Object -Property SenderPrivateBytes -Maximum).Maximum)
        ReceiverPrivateHighWaterBytes=[Int64](($Samples | Measure-Object -Property ReceiverPrivateBytes -Maximum).Maximum)
        SenderHandleHighWater=[Int64](($Samples | Measure-Object -Property SenderHandles -Maximum).Maximum)
        ReceiverHandleHighWater=[Int64](($Samples | Measure-Object -Property ReceiverHandles -Maximum).Maximum)
        LongSoakVerified=$false
        SenderPrivateMedianDriftBytes=[Int64]0
        ReceiverPrivateMedianDriftBytes=[Int64]0
        SenderPrivateHighWaterIncreaseBytes=[Int64]0
        ReceiverPrivateHighWaterIncreaseBytes=[Int64]0
        SenderHandleMedianDrift=[Int64]0
        ReceiverHandleMedianDrift=[Int64]0
        SenderHandleHighWaterIncrease=[Int64]0
        ReceiverHandleHighWaterIncrease=[Int64]0
    }
    if ($Policy.SoakSeconds -eq 0)
    {
        return [pscustomobject]$summary
    }
    if ($null -eq $PublishedMilliseconds)
    {
        throw 'Long-soak run never observed atomic file publication'
    }
    $publication = [Int64]$PublishedMilliseconds
    if ($publication -lt 0 -or $publication -gt [Int64]$last.ElapsedMilliseconds)
    {
        throw 'Long-soak publication timestamp is outside the paired sample interval'
    }
    $warmupStart = $publication + [Int64]$Policy.SoakWarmupSeconds * 1000
    $baselineEnd = $warmupStart + [Int64]$Policy.SoakComparisonWindowSeconds * 1000
    $finalStart = $publication + ([Int64]$Policy.SoakSeconds - [Int64]$Policy.SoakComparisonWindowSeconds) * 1000
    $baseline = @($Samples | Where-Object { [Int64]$_.ElapsedMilliseconds -ge $warmupStart -and [Int64]$_.ElapsedMilliseconds -lt $baselineEnd })
    $final = @($Samples | Where-Object { [Int64]$_.ElapsedMilliseconds -ge $finalStart })
    $postWarmup = @($Samples | Where-Object { [Int64]$_.ElapsedMilliseconds -ge $warmupStart })
    if ($baseline.Count -lt 20 -or $final.Count -lt 20 -or $postWarmup.Count -lt 200)
    {
        throw "Long-soak resource windows are incomplete: baseline=$($baseline.Count) final=$($final.Count) postWarmup=$($postWarmup.Count)"
    }
    foreach ($processName in @('Sender','Receiver'))
    {
        $privateProperty = "${processName}PrivateBytes"
        $handleProperty = "${processName}Handles"
        $privateBaseline = Get-Phase1MedianInt64 @($baseline | ForEach-Object { $_.$privateProperty })
        $privateFinal = Get-Phase1MedianInt64 @($final | ForEach-Object { $_.$privateProperty })
        $handleBaseline = Get-Phase1MedianInt64 @($baseline | ForEach-Object { $_.$handleProperty })
        $handleFinal = Get-Phase1MedianInt64 @($final | ForEach-Object { $_.$handleProperty })
        $privateHighWater = [Int64](($postWarmup | Measure-Object -Property $privateProperty -Maximum).Maximum)
        $handleHighWater = [Int64](($postWarmup | Measure-Object -Property $handleProperty -Maximum).Maximum)
        $summary["${processName}PrivateMedianDriftBytes"] = $privateFinal - $privateBaseline
        $summary["${processName}PrivateHighWaterIncreaseBytes"] = $privateHighWater - $privateBaseline
        $summary["${processName}HandleMedianDrift"] = $handleFinal - $handleBaseline
        $summary["${processName}HandleHighWaterIncrease"] = $handleHighWater - $handleBaseline
    }
    $summary.LongSoakVerified = $true
    return [pscustomobject]$summary
}

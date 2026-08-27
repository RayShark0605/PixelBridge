[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GateExecutable,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [ValidateSet('fast', 'resume', 'large')][string[]]$Modes = @('fast', 'resume', 'large'),
    [ValidateSet('fast,resume', 'fast,resume,large')][string]$ModesCsv
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if ($ModesCsv) { $Modes = $ModesCsv.Split(',') }
$GateExecutable = (Resolve-Path -LiteralPath $GateExecutable).Path
$EvidenceDirectory = [System.IO.Path]::GetFullPath($EvidenceDirectory)
[System.IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null

foreach ($round in 1..2)
{
    foreach ($mode in $Modes)
    {
        $prefix = Join-Path $EvidenceDirectory "round-$round-$mode"
        $scratch = $prefix + '-scratch'
        if (Test-Path -LiteralPath $scratch)
        {
            throw "Replay requires a new empty scratch path: $scratch"
        }
        [System.IO.Directory]::CreateDirectory($scratch) | Out-Null
        & $GateExecutable --mode $mode --scratch $scratch --evidence ($prefix + '.jsonl') 2>&1 |
            Tee-Object -FilePath ($prefix + '.log')
        if ($LASTEXITCODE -ne 0)
        {
            throw "Reference Gate failed; scratch retained at $scratch"
        }
        $records = @(Get-Content -LiteralPath ($prefix + '.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
        if (@($records | Where-Object { $_.status -ne 'pass' }).Count -ne 0 -or
            @($records | Where-Object { $_.case -eq 'phase0-gate-summary' -and $_.mode -eq $mode }).Count -ne 1)
        {
            throw "Missing successful Gate summary: $prefix"
        }
        if (@(Get-ChildItem -LiteralPath $scratch -Force).Count -ne 0)
        {
            throw "Successful Gate retained temporary payload files in $scratch"
        }
    }
}

$comparisons = foreach ($mode in $Modes)
{
    $normalized = foreach ($round in 1..2)
    {
        $lines = Get-Content -LiteralPath (Join-Path $EvidenceDirectory "round-$round-$mode.jsonl")
        $stableLines = foreach ($line in $lines)
        {
            $record = $line | ConvertFrom-Json
            $record.PSObject.Properties.Remove('elapsed_ms')
            $record | ConvertTo-Json -Compress -Depth 10
        }
        ,@($stableLines)
    }
    if (($normalized[0] -join "`n") -cne ($normalized[1] -join "`n"))
    {
        throw "Deterministic evidence differs between clean rounds for $mode"
    }
    [ordered]@{ mode = $mode; status = 'pass'; rounds = 2; stable_records = $normalized[0].Count }
}
$comparisonPath = Join-Path $EvidenceDirectory 'replay-comparison.json'
@($comparisons) | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $comparisonPath -Encoding utf8
Write-Output "REFERENCE_REPLAY_IDENTICAL $comparisonPath"

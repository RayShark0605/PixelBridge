#Requires -Version 7.2
param([Parameter(Mandatory)][string]$ScratchRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'GateJson.ps1')

$failedFileResult = [pscustomobject]@{ Gate='FAIL'; Backend='wgc'; Profile='desktop-levels-2x2'; Error='expected failure shape' }
if (Test-Phase1SuccessfulFileResult $failedFileResult) { throw 'A failed physical file report without Receiver/Resources was accepted as projectable' }
$passedFileResult = [pscustomobject]@{ Gate='PASS'; Receiver=[pscustomobject]@{}; Resources=[pscustomobject]@{} }
if (-not (Test-Phase1SuccessfulFileResult $passedFileResult)) { throw 'A complete passing physical file report was rejected as projectable' }

$directory = Join-Path $ScratchRoot ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($directory) | Out-Null

$emptyPath = Join-Path $directory 'empty.json'
Write-NewJson $emptyPath ([object[]]@())
$emptyBytes = [IO.File]::ReadAllBytes($emptyPath)
if ([Text.Encoding]::UTF8.GetString($emptyBytes) -cne '[]')
{
    throw 'Empty collection was not serialized as one JSON document'
}
if ($emptyBytes.Length -ge 3 -and $emptyBytes[0] -eq 0xef -and $emptyBytes[1] -eq 0xbb -and $emptyBytes[2] -eq 0xbf)
{
    throw 'Gate JSON must be UTF-8 without BOM'
}

$objectPath = Join-Path $directory 'object.json'
$identity = [ordered]@{ Head='abc'; Worktree=[object[]]@(); Counter=[UInt64]::MaxValue }
Write-NewJson $objectPath $identity
$parsed = Get-Content -LiteralPath $objectPath -Raw | ConvertFrom-Json
if ($parsed.Head -cne 'abc' -or @($parsed.Worktree).Count -ne 0 -or [string]$parsed.Counter -cne [string][UInt64]::MaxValue)
{
    throw 'Gate JSON object did not round-trip'
}

$overwriteRejected = $false
try
{
    Write-NewJson $objectPath @{ Replaced=$true }
}
catch
{
    $overwriteRejected = $true
}
if (-not $overwriteRejected)
{
    throw 'Gate JSON silently overwrote an existing evidence artifact'
}
$unchanged = Get-Content -LiteralPath $objectPath -Raw | ConvertFrom-Json
if ($unchanged.Head -cne 'abc')
{
    throw 'Rejected overwrite changed the original evidence artifact'
}

$sharedPath = Join-Path $directory 'shared.jsonl'
$sharedWriter = [IO.File]::Open($sharedPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try
{
    $firstRecord = [Text.UTF8Encoding]::new($false).GetBytes("{`"event`":`"snapshot`",`"value`":1}`n")
    $sharedWriter.Write($firstRecord, 0, $firstRecord.Length)
    $sharedWriter.Flush($true)
    $sharedLines = @(Read-SharedUtf8Lines $sharedPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($sharedLines.Count -ne 1 -or ($sharedLines[0] | ConvertFrom-Json).value -ne 1)
    {
        throw 'Live shared JSONL record did not round-trip'
    }
}
finally
{
    $sharedWriter.Dispose()
}

$eventRecord = '{"event":"final","value":1}' | ConvertFrom-Json
$diagnosticRecord = '{"scope":"presentation"}' | ConvertFrom-Json
if ((Get-JsonEventName $eventRecord) -cne 'final' -or
    $null -ne (Get-JsonEventName $diagnosticRecord) -or
    $null -ne (Get-JsonEventName 7) -or
    $null -ne (Get-JsonEventName $null))
{
    throw 'Optional JSONL event lookup did not preserve mixed-schema records'
}

Write-Output 'DESKTOP_LEVELS_GATE_JSON_PASS empty=1 object=1 create-only=1 utf8-no-bom=1 live-shared-read=1 optional-event=1'

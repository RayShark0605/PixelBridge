#Requires -Version 7.2

function Test-Phase1SuccessfulFileResult($Result)
{
    if ($null -eq $Result -or $null -eq $Result.PSObject.Properties['Gate'] -or $Result.Gate -cne 'PASS')
    {
        return $false
    }
    return $null -ne $Result.PSObject.Properties['Receiver'] -and
        $null -ne $Result.PSObject.Properties['Resources']
}

function Write-NewJson([string]$Path, $Value)
{
    # -InputObject is deliberate: piping an empty array supplies no pipeline
    # input, so ConvertTo-Json returns $null and a clean worktree makes the
    # Gate fail before its first command can run.
    $json = ConvertTo-Json -InputObject $Value -Depth 24
    if ($null -eq $json)
    {
        throw 'JSON serialization produced no document'
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try
    {
        $file.Write($bytes, 0, $bytes.Length)
        $file.Flush($true)
    }
    finally
    {
        $file.Dispose()
    }
}

function Read-SharedUtf8Lines([string]$Path)
{
    # The native JSONL writer deliberately grants FILE_SHARE_READ while it is
    # alive. A reader must in turn grant write sharing; File.ReadAllLines uses
    # a narrower share mode and therefore races with the live writer on
    # Windows. A trailing record may still be incomplete and is left for the
    # caller to ignore until the next poll.
    $file = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try
    {
        $reader = [IO.StreamReader]::new($file, [Text.UTF8Encoding]::new($false, $false), $true, 4096, $true)
        try
        {
            $text = $reader.ReadToEnd()
        }
        finally
        {
            $reader.Dispose()
        }
    }
    finally
    {
        $file.Dispose()
    }
    if ($text.Length -eq 0)
    {
        return [string[]]@()
    }
    return [regex]::Split($text, "`r?`n")
}

function Get-JsonEventName([object]$Value)
{
    # A JSONL stream can deliberately contain multiple schemas. Under
    # StrictMode, direct access to a missing .event property throws before a
    # caller can skip a non-event diagnostic record, so inspect the dynamic
    # property bag without weakening JSON parsing of the final record.
    if ($null -eq $Value)
    {
        return $null
    }
    $property = $Value.PSObject.Properties['event']
    if ($null -eq $property -or $null -eq $property.Value)
    {
        return $null
    }
    return [string]$property.Value
}

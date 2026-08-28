function Get-CppcheckSourceHash([string]$Path)
{
    # Git autocrlf must not invalidate a semantic review after a fresh checkout.
    $text = [IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text)))
}

function Get-CppcheckReview([string]$ReportPath, [int]$ExitCode, [string]$SourceRoot, [string]$ReviewPath)
{
    [xml]$report = Get-Content -LiteralPath $ReportPath -Raw
    $review = Get-Content -LiteralPath $ReviewPath -Raw | ConvertFrom-Json
    if ($ExitCode -notin @(0,2)) { throw 'Cppcheck execution failed' }
    $version = $report.SelectSingleNode('/results/cppcheck')
    $errors = $report.SelectSingleNode('/results/errors')
    if ($null -eq $version -or $null -eq $errors) { throw 'Incomplete cppcheck XML report' }
    if ($version.GetAttribute('version') -ne $review.Cppcheck) { throw 'Cppcheck version changed; findings need a new review' }
    # The PowerShell XML property adapter represents <errors/> as a string.
    # XPath retains the real element, including an explicitly empty result.
    $findings = @($errors.SelectNodes('error'))
    if ($ExitCode -eq 2 -and $findings.Count -eq 0) { throw 'Cppcheck failed without a diagnostic to review' }
    foreach ($finding in $findings)
    {
        $location = @($finding.location)[0]
        $absolute = [IO.Path]::GetFullPath($location.file, $SourceRoot)
        $path = [IO.Path]::GetRelativePath($SourceRoot, $absolute).Replace('\','/')
        if ($path -eq '..' -or $path.StartsWith('../') -or [IO.Path]::IsPathRooted($path))
        {
            throw 'Static diagnostic refers outside the reviewed source root'
        }
        $match = @($review.Entries | Where-Object { $_.File -ceq $path -and $_.Line -eq [int]$location.line -and
            $_.Id -ceq $finding.id -and $_.Severity -ceq $finding.severity -and $_.Message -ceq $finding.msg })
        if ($match.Count -ne 1 -or (Get-CppcheckSourceHash $absolute) -cne $match[0].SourceSHA256)
        {
            throw "New/changed/unreviewed static diagnostic: $path : $($location.line) $($finding.id)"
        }
    }
    return @{ Findings=$findings.Count; ExactReviewedBaseline=$true; ReviewManifest=$ReviewPath; RawReport=$ReportPath }
}

#Requires -Version 7.0

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1') -ErrorAction Stop

function Assert-PBStep22ExactKeys
{
    param(
        [Parameter(Mandatory = $true)][object]$Dictionary,
        [Parameter(Mandatory = $true)][string[]]$ExpectedKeys,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Dictionary -isnot [System.Collections.IDictionary])
    {
        throw "$Name must be a JSON object"
    }
    $actualKeys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($key in $Dictionary.Keys)
    {
        if (-not $actualKeys.Add([string]$key))
        {
            throw "$Name contains duplicate key '$key'"
        }
    }
    if ($actualKeys.Count -ne $ExpectedKeys.Count)
    {
        throw "$Name does not contain the exact required key set"
    }
    foreach ($key in $ExpectedKeys)
    {
        if (-not $actualKeys.Remove($key))
        {
            throw "$Name is missing exact key '$key'"
        }
    }
}

function Get-PBStep22UInt64
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Value -or $Value -is [bool] -or $Value -is [string])
    {
        throw "$Name must be a non-negative UInt64 JSON integer"
    }
    try
    {
        if ($Value -is [System.Numerics.BigInteger])
        {
            $maximum = [System.Numerics.BigInteger]::Parse([UInt64]::MaxValue.ToString([Globalization.CultureInfo]::InvariantCulture), [Globalization.CultureInfo]::InvariantCulture)
            if ($Value -lt [System.Numerics.BigInteger]::Zero -or $Value -gt $maximum)
            {
                throw 'out of range'
            }
            return [UInt64]$Value
        }
        $typeCode = [Type]::GetTypeCode($Value.GetType())
        if ($typeCode -notin @([TypeCode]::Byte, [TypeCode]::SByte, [TypeCode]::Int16, [TypeCode]::UInt16,
            [TypeCode]::Int32, [TypeCode]::UInt32, [TypeCode]::Int64, [TypeCode]::UInt64))
        {
            throw 'not an integer'
        }
        $number = [decimal]$Value
        if ($number -lt 0 -or $number -gt [decimal][UInt64]::MaxValue)
        {
            throw 'out of range'
        }
        return [UInt64]$number
    }
    catch
    {
        throw "$Name must be a non-negative UInt64 JSON integer"
    }
}

function Assert-PBStep22Hash
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [ValidateSet(32, 40, 64)][int]$Length = 64
    )
    if ($Value -isnot [string] -or [string]$Value -cnotmatch "^[0-9a-f]{$Length}$")
    {
        throw "$Name must be exactly $Length lowercase hexadecimal characters"
    }
}

function Assert-PBStep22String
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [ValidateRange(1, 4096)][int]$MaximumLength = 256
    )
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$Value) -or
        ([string]$Value).Length -gt $MaximumLength -or ([string]$Value).IndexOf([char]0) -ge 0)
    {
        throw "$Name must be a non-empty bounded string"
    }
}

function Get-PBStep22RequiredRunDefinitions
{
    return @(
        [ordered]@{ ordinal = [UInt32]1; slotId = 'random-1mib-01'; sourceClass = 'Random1MiB'; sourceRelativePath = 'random-1MiB.bin'; successOrdinalWithinSource = [UInt32]1 },
        [ordered]@{ ordinal = [UInt32]2; slotId = 'random-1mib-02'; sourceClass = 'Random1MiB'; sourceRelativePath = 'random-1MiB.bin'; successOrdinalWithinSource = [UInt32]2 },
        [ordered]@{ ordinal = [UInt32]3; slotId = 'random-1mib-03'; sourceClass = 'Random1MiB'; sourceRelativePath = 'random-1MiB.bin'; successOrdinalWithinSource = [UInt32]3 },
        [ordered]@{ ordinal = [UInt32]4; slotId = 'random-8mib-01'; sourceClass = 'Random8MiB'; sourceRelativePath = 'random-8MiB.bin'; successOrdinalWithinSource = [UInt32]1 },
        [ordered]@{ ordinal = [UInt32]5; slotId = 'random-8mib-02'; sourceClass = 'Random8MiB'; sourceRelativePath = 'random-8MiB.bin'; successOrdinalWithinSource = [UInt32]2 },
        [ordered]@{ ordinal = [UInt32]6; slotId = 'zip-4mib-payload-01'; sourceClass = 'ZipWithRandom4MiBPayload'; sourceRelativePath = 'random-payload-4MiB.zip'; successOrdinalWithinSource = [UInt32]1 }
    )
}

function Assert-PBStep22Application
{
    param(
        [Parameter(Mandatory = $true)][object]$Application,
        [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'Decoder')][string]$Role
    )
    Assert-PBStep22ExactKeys -Dictionary $Application `
        -ExpectedKeys @('role', 'application', 'relativeExecutablePath', 'size', 'sha256', 'versionOutput') `
        -Name "Step 22 $Role application"
    $expectedApplication = "PixelBridge$Role"
    $expectedPath = "$Role/PixelBridge$Role.exe"
    if ($Application.role -isnot [string] -or [string]$Application.role -cne $Role -or
        $Application.application -isnot [string] -or [string]$Application.application -cne $expectedApplication -or
        $Application.relativeExecutablePath -isnot [string] -or [string]$Application.relativeExecutablePath -cne $expectedPath -or
        (Get-PBStep22UInt64 -Value $Application.size -Name "$Role application size") -eq 0)
    {
        throw "Step 22 $Role application identity is invalid"
    }
    Assert-PBStep22Hash -Value $Application.sha256 -Name "$Role application SHA-256"
    Assert-PBStep22String -Value $Application.versionOutput -Name "$Role version output" -MaximumLength 256
}

function Import-PBRemoteVisualStep22Campaign
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256) -and
        ([string]$ExpectedSha256 -cnotmatch '^[0-9a-f]{64}$' -or [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 22 campaign identity does not match the expected SHA-256'
    }
    $campaign = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 2MB
    Assert-PBStep22ExactKeys -Dictionary $campaign `
        -ExpectedKeys @('schema', 'createdUtc', 'status', 'campaignId', 'runtime', 'sourceSet', 'profile', 'policy', 'runs', 'acceptance', 'truthBoundary') `
        -Name 'Step 22 campaign'
    if ($campaign.schema -isnot [string] -or [string]$campaign.schema -cne 'PixelBridge.RemoteVisualStep22Campaign.1' -or
        $campaign.status -isnot [string] -or [string]$campaign.status -cne 'READY_NOT_EXECUTED')
    {
        throw 'Step 22 campaign schema or readiness status is invalid'
    }
    if ($campaign.createdUtc -is [DateTime])
    {
        if (([DateTime]$campaign.createdUtc).Kind -ne [DateTimeKind]::Utc)
        {
            throw 'Step 22 campaign createdUtc must be canonical UTC round-trip text'
        }
        $createdUtc = [DateTimeOffset]::new([DateTime]$campaign.createdUtc)
    }
    elseif ($campaign.createdUtc -is [string])
    {
        try
        {
            $createdUtc = [DateTimeOffset]::ParseExact([string]$campaign.createdUtc, 'o',
                [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind)
        }
        catch
        {
            throw 'Step 22 campaign createdUtc must be canonical UTC round-trip text'
        }
    }
    else
    {
        throw 'Step 22 campaign createdUtc must be canonical UTC round-trip text'
    }
    if ($createdUtc.Offset -ne [TimeSpan]::Zero)
    {
        throw 'Step 22 campaign createdUtc must be canonical UTC round-trip text'
    }
    Assert-PBStep22Hash -Value $campaign.campaignId -Name 'campaignId' -Length 32

    $runtime = $campaign.runtime
    Assert-PBStep22ExactKeys -Dictionary $runtime `
        -ExpectedKeys @('gitCommit', 'headTree', 'packageManifestSha256', 'packageSealSha256', 'buildIdentityFingerprintSha256', 'packagePayloadFingerprintSha256', 'applications') `
        -Name 'Step 22 runtime'
    Assert-PBStep22Hash -Value $runtime.gitCommit -Name 'runtime gitCommit' -Length 40
    Assert-PBStep22Hash -Value $runtime.headTree -Name 'runtime headTree' -Length 40
    foreach ($name in @('packageManifestSha256', 'packageSealSha256', 'buildIdentityFingerprintSha256', 'packagePayloadFingerprintSha256'))
    {
        Assert-PBStep22Hash -Value $runtime[$name] -Name "runtime $name"
    }
    if ($runtime.applications -isnot [System.Collections.IList] -or @($runtime.applications).Count -ne 2)
    {
        throw 'Step 22 runtime must bind exactly two applications'
    }
    foreach ($role in @('Encoder', 'Decoder'))
    {
        $matches = @($runtime.applications | Where-Object { $_ -is [System.Collections.IDictionary] -and [string]$_.role -ceq $role })
        if ($matches.Count -ne 1)
        {
            throw "Step 22 runtime must bind exactly one $role application"
        }
        Assert-PBStep22Application -Application $matches[0] -Role $role
    }

    $sourceSet = $campaign.sourceSet
    Assert-PBStep22ExactKeys -Dictionary $sourceSet `
        -ExpectedKeys @('sourceSetId', 'sourceSetFingerprintSha256', 'manifestSha256', 'sealSha256', 'files', 'zipSourcePayload') `
        -Name 'Step 22 source set'
    Assert-PBStep22Hash -Value $sourceSet.sourceSetId -Name 'sourceSetId' -Length 32
    foreach ($name in @('sourceSetFingerprintSha256', 'manifestSha256', 'sealSha256'))
    {
        Assert-PBStep22Hash -Value $sourceSet[$name] -Name "source set $name"
    }
    if ($sourceSet.files -isnot [System.Collections.IList] -or @($sourceSet.files).Count -ne 3)
    {
        throw 'Step 22 source set must contain exactly three files'
    }
    $expectedSourcePaths = @('random-1MiB.bin', 'random-8MiB.bin', 'random-payload-4MiB.zip')
    $sourceMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    foreach ($source in @($sourceSet.files))
    {
        Assert-PBStep22ExactKeys -Dictionary $source `
            -ExpectedKeys @('relativePath', 'size', 'sha256', 'pixelBridgeSegmentCompression') -Name 'Step 22 source file'
        if ($source.relativePath -isnot [string] -or [string]$source.relativePath -cnotin $expectedSourcePaths -or
            -not $sourceMap.TryAdd([string]$source.relativePath, $source) -or
            $source.pixelBridgeSegmentCompression -isnot [string] -or
            [string]$source.pixelBridgeSegmentCompression -cne 'RAW/OFF')
        {
            throw 'Step 22 source file path, uniqueness, or RAW/OFF contract is invalid'
        }
        $sourceSize = Get-PBStep22UInt64 -Value $source.size -Name 'source file size'
        if ($sourceSize -eq 0 -or $sourceSize -gt 8MB)
        {
            throw 'Step 22 source file size is outside the one-Segment product boundary'
        }
        Assert-PBStep22Hash -Value $source.sha256 -Name 'source file SHA-256'
    }
    if ($sourceMap.Count -ne 3 -or
        (Get-PBStep22UInt64 -Value $sourceMap['random-1MiB.bin'].size -Name '1 MiB source size') -ne 1MB -or
        (Get-PBStep22UInt64 -Value $sourceMap['random-8MiB.bin'].size -Name '8 MiB source size') -ne 8MB)
    {
        throw 'Step 22 fixed random source sizes are invalid'
    }
    $zipPayload = $sourceSet.zipSourcePayload
    Assert-PBStep22ExactKeys -Dictionary $zipPayload `
        -ExpectedKeys @('archiveRelativePath', 'entryPath', 'size', 'sha256') -Name 'Step 22 ZIP payload'
    if ($zipPayload.archiveRelativePath -isnot [string] -or [string]$zipPayload.archiveRelativePath -cne 'random-payload-4MiB.zip' -or
        $zipPayload.entryPath -isnot [string] -or [string]$zipPayload.entryPath -cne 'payload.bin' -or
        (Get-PBStep22UInt64 -Value $zipPayload.size -Name 'ZIP payload size') -ne 4MB)
    {
        throw 'Step 22 ZIP payload binding is invalid'
    }
    Assert-PBStep22Hash -Value $zipPayload.sha256 -Name 'ZIP payload SHA-256'

    $profile = $campaign.profile
    Assert-PBStep22ExactKeys -Dictionary $profile `
        -ExpectedKeys @('token', 'name', 'visualProfileId', 'visualLayoutVersion', 'codedDataBytesPerFrame', 'codewordsPerFrame', 'outerFec', 'innerFec') `
        -Name 'Step 22 profile'
    if ($profile.token -isnot [string] -or [string]$profile.token -cne 'remote-lf4' -or
        $profile.name -isnot [string] -or [string]$profile.name -cne 'PB-RemoteVisual-LF4-X1 (Experimental)' -or
        (Get-PBStep22UInt64 -Value $profile.visualProfileId -Name 'visualProfileId') -ne [UInt64]5783275402097472561 -or
        (Get-PBStep22UInt64 -Value $profile.visualLayoutVersion -Name 'visualLayoutVersion') -ne 7 -or
        (Get-PBStep22UInt64 -Value $profile.codedDataBytesPerFrame -Name 'codedDataBytesPerFrame') -ne 8100 -or
        (Get-PBStep22UInt64 -Value $profile.codewordsPerFrame -Name 'codewordsPerFrame') -ne 4 -or
        $profile.outerFec -isnot [string] -or [string]$profile.outerFec -cne 'Wirehair V2' -or
        $profile.innerFec -isnot [string] -or [string]$profile.innerFec -cne 'Robust DVB-S2 Short QC-LDPC')
    {
        throw 'Step 22 profile/FEC identity is invalid'
    }

    $policy = $campaign.policy
    Assert-PBStep22ExactKeys -Dictionary $policy `
        -ExpectedKeys @('configurationName', 'channelType', 'remoteProvider', 'compression', 'segmentCompression', 'captureBackend', 'encoderMonitorSelector', 'decoderRoiSelection', 'logicalVisualFps', 'controlRepetitions', 'decoderTimeoutSeconds', 'noProgressSeconds', 'replayPolicy', 'decoderStartsBeforeEncoder', 'manualSenderStopAfterReceiverCompletion', 'noNonVisualPayloadPath') `
        -Name 'Step 22 execution policy'
    if ($policy.configurationName -isnot [string] -or [string]$policy.configurationName -cne 'Step21ProvenReliableLF4TwoHertz' -or
        $policy.channelType -isnot [string] -or [string]$policy.channelType -cne 'RemoteVisual' -or
        $policy.remoteProvider -isnot [string] -or [string]$policy.remoteProvider -cne 'UserProvidedVisualLink' -or
        $policy.compression -isnot [string] -or [string]$policy.compression -cne 'off' -or
        $policy.segmentCompression -isnot [string] -or [string]$policy.segmentCompression -cne 'RAW/OFF' -or
        $policy.captureBackend -isnot [string] -or [string]$policy.captureBackend -cne 'wgc' -or
        $policy.encoderMonitorSelector -isnot [string] -or [string]$policy.encoderMonitorSelector -cne 'primary' -or
        $policy.decoderRoiSelection -isnot [string] -or [string]$policy.decoderRoiSelection -cne 'WholeExperimentMonitorAtRunStart' -or
        (Get-PBStep22UInt64 -Value $policy.logicalVisualFps -Name 'logicalVisualFps') -ne 2 -or
        (Get-PBStep22UInt64 -Value $policy.controlRepetitions -Name 'controlRepetitions') -ne 12 -or
        (Get-PBStep22UInt64 -Value $policy.decoderTimeoutSeconds -Name 'decoderTimeoutSeconds') -ne 3600 -or
        (Get-PBStep22UInt64 -Value $policy.noProgressSeconds -Name 'noProgressSeconds') -ne 180 -or
        $policy.replayPolicy -isnot [string] -or [string]$policy.replayPolicy -cne 'DisabledForBoundedLiveFileSmoke' -or
        $policy.decoderStartsBeforeEncoder -isnot [bool] -or -not [bool]$policy.decoderStartsBeforeEncoder -or
        $policy.manualSenderStopAfterReceiverCompletion -isnot [bool] -or -not [bool]$policy.manualSenderStopAfterReceiverCompletion -or
        $policy.noNonVisualPayloadPath -isnot [bool] -or -not [bool]$policy.noNonVisualPayloadPath)
    {
        throw 'Step 22 execution policy differs from the frozen reliable configuration'
    }

    $requiredRuns = @(Get-PBStep22RequiredRunDefinitions)
    if ($campaign.runs -isnot [System.Collections.IList] -or @($campaign.runs).Count -ne $requiredRuns.Count)
    {
        throw 'Step 22 campaign must contain exactly six scheduled runs'
    }
    $runIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    for ($index = 0; $index -lt $requiredRuns.Count; $index++)
    {
        $run = @($campaign.runs)[$index]
        $expectedRun = $requiredRuns[$index]
        Assert-PBStep22ExactKeys -Dictionary $run `
            -ExpectedKeys @('ordinal', 'slotId', 'runId', 'status', 'sourceClass', 'sourceRelativePath', 'sourceSize', 'sourceSha256', 'successOrdinalWithinSource', 'evidenceDirectoryName') `
            -Name "Step 22 run $($index + 1)"
        $runOrdinal = Get-PBStep22UInt64 -Value $run.ordinal -Name 'run ordinal'
        $successOrdinal = Get-PBStep22UInt64 -Value $run.successOrdinalWithinSource -Name 'source success ordinal'
        if ($runOrdinal -ne [UInt64]$expectedRun.ordinal -or $successOrdinal -ne [UInt64]$expectedRun.successOrdinalWithinSource -or
            $run.slotId -isnot [string] -or [string]$run.slotId -cne [string]$expectedRun.slotId -or
            $run.sourceClass -isnot [string] -or [string]$run.sourceClass -cne [string]$expectedRun.sourceClass -or
            $run.sourceRelativePath -isnot [string] -or [string]$run.sourceRelativePath -cne [string]$expectedRun.sourceRelativePath -or
            $run.status -isnot [string] -or [string]$run.status -cne 'PENDING')
        {
            throw "Step 22 run $($index + 1) schedule identity is invalid"
        }
        Assert-PBStep22Hash -Value $run.runId -Name "run $($index + 1) RunId" -Length 32
        if (-not $runIds.Add([string]$run.runId) -or $run.evidenceDirectoryName -isnot [string] -or
            [string]$run.evidenceDirectoryName -cne [string]$run.runId)
        {
            throw 'Step 22 run IDs must be unique and equal their evidence-directory names'
        }
        $source = $sourceMap[[string]$run.sourceRelativePath]
        if ((Get-PBStep22UInt64 -Value $run.sourceSize -Name 'run source size') -ne
            (Get-PBStep22UInt64 -Value $source.size -Name 'manifest source size') -or
            $run.sourceSha256 -isnot [string] -or [string]$run.sourceSha256 -cne [string]$source.sha256)
        {
            throw 'Step 22 run source identity differs from the sealed source set'
        }
    }

    $acceptance = $campaign.acceptance
    Assert-PBStep22ExactKeys -Dictionary $acceptance `
        -ExpectedKeys @('requiredRunCount', 'random1MiBSuccessesRequired', 'random8MiBSuccessesRequired', 'zip4MiBPayloadSuccessesRequired', 'allScheduledRunsMustPass', 'preserveFailedEvidence', 'reuseRunIdForbidden', 'senderCyclePositionIsCompletion', 'remoteVisualSmokePassRequiresFinalVerifier') `
        -Name 'Step 22 acceptance'
    if ((Get-PBStep22UInt64 -Value $acceptance.requiredRunCount -Name 'requiredRunCount') -ne 6 -or
        (Get-PBStep22UInt64 -Value $acceptance.random1MiBSuccessesRequired -Name '1 MiB quota') -ne 3 -or
        (Get-PBStep22UInt64 -Value $acceptance.random8MiBSuccessesRequired -Name '8 MiB quota') -ne 2 -or
        (Get-PBStep22UInt64 -Value $acceptance.zip4MiBPayloadSuccessesRequired -Name 'ZIP quota') -ne 1 -or
        $acceptance.allScheduledRunsMustPass -isnot [bool] -or -not [bool]$acceptance.allScheduledRunsMustPass -or
        $acceptance.preserveFailedEvidence -isnot [bool] -or -not [bool]$acceptance.preserveFailedEvidence -or
        $acceptance.reuseRunIdForbidden -isnot [bool] -or -not [bool]$acceptance.reuseRunIdForbidden -or
        $acceptance.senderCyclePositionIsCompletion -isnot [bool] -or [bool]$acceptance.senderCyclePositionIsCompletion -or
        $acceptance.remoteVisualSmokePassRequiresFinalVerifier -isnot [bool] -or -not [bool]$acceptance.remoteVisualSmokePassRequiresFinalVerifier)
    {
        throw 'Step 22 success quotas or evidence-retention contract is invalid'
    }

    $truth = $campaign.truthBoundary
    Assert-PBStep22ExactKeys -Dictionary $truth `
        -ExpectedKeys @('executedRunCount', 'successfulRunCount', 'remoteVisualSmokePass', 'certifiedRemoteVisualProfile', 'crossBrandCoverageClaimed', 'crossModeCoverageClaimed', 'statement') `
        -Name 'Step 22 truth boundary'
    if ((Get-PBStep22UInt64 -Value $truth.executedRunCount -Name 'executedRunCount') -ne 0 -or
        (Get-PBStep22UInt64 -Value $truth.successfulRunCount -Name 'successfulRunCount') -ne 0 -or
        $truth.remoteVisualSmokePass -isnot [bool] -or [bool]$truth.remoteVisualSmokePass -or
        $truth.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$truth.certifiedRemoteVisualProfile -or
        $truth.crossBrandCoverageClaimed -isnot [bool] -or [bool]$truth.crossBrandCoverageClaimed -or
        $truth.crossModeCoverageClaimed -isnot [bool] -or [bool]$truth.crossModeCoverageClaimed)
    {
        throw 'Step 22 readiness artifact inflates execution, smoke, matrix, or certification truth'
    }
    Assert-PBStep22String -Value $truth.statement -Name 'truth-boundary statement' -MaximumLength 1024
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $campaign }
}

Export-ModuleMember -Function Get-PBStep22RequiredRunDefinitions, Import-PBRemoteVisualStep22Campaign

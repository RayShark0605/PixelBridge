#Requires -Version 7.0

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-PBStrictJsonElement
{
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Path
    )
    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object)
    {
        $propertyNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($property in $Element.EnumerateObject())
        {
            if (-not $propertyNames.Add($property.Name))
            {
                throw "JSON contains duplicate property '$($property.Name)' at $Path"
            }
            Assert-PBStrictJsonElement -Element $property.Value -Path "$Path.$($property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array)
    {
        $index = 0
        foreach ($item in $Element.EnumerateArray())
        {
            Assert-PBStrictJsonElement -Element $item -Path "$Path[$index]"
            $index++
        }
    }
}

function ConvertFrom-PBStrictJsonText
{
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [string]$Name = 'JSON input'
    )
    if ([string]::IsNullOrWhiteSpace($Text))
    {
        throw "$Name is empty"
    }
    $options = [System.Text.Json.JsonDocumentOptions]::new()
    $options.AllowTrailingCommas = $false
    $options.CommentHandling = [System.Text.Json.JsonCommentHandling]::Disallow
    $options.MaxDepth = 100
    $document = $null
    try
    {
        $document = [System.Text.Json.JsonDocument]::Parse($Text, $options)
        Assert-PBStrictJsonElement -Element $document.RootElement -Path '$'
        $value = $Text | ConvertFrom-Json -AsHashtable -Depth 100
    }
    catch
    {
        throw "$Name is malformed or ambiguous: $($_.Exception.Message)"
    }
    finally
    {
        if ($null -ne $document)
        {
            $document.Dispose()
        }
    }
    return $value
}

function Assert-PBPilotExactKeys
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

function Get-PBPilotFiniteDouble
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Value -or $Value -is [bool] -or $Value -isnot [ValueType])
    {
        throw "$Name must be a finite JSON number"
    }
    try
    {
        $typeCode = [Type]::GetTypeCode($Value.GetType())
        if ($typeCode -notin @([TypeCode]::Byte, [TypeCode]::SByte, [TypeCode]::Int16, [TypeCode]::UInt16,
            [TypeCode]::Int32, [TypeCode]::UInt32, [TypeCode]::Int64, [TypeCode]::UInt64,
            [TypeCode]::Single, [TypeCode]::Double, [TypeCode]::Decimal))
        {
            throw 'unsupported numeric type'
        }
        $number = [double]$Value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number))
        {
            throw 'non-finite number'
        }
        return $number
    }
    catch
    {
        throw "$Name must be a finite JSON number"
    }
}

function Get-PBPilotUInt32
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Value -or $Value -is [bool] -or $Value -isnot [ValueType])
    {
        throw "$Name must be a non-negative UInt32 integer"
    }
    try
    {
        $typeCode = [Type]::GetTypeCode($Value.GetType())
        if ($typeCode -notin @([TypeCode]::Byte, [TypeCode]::SByte, [TypeCode]::Int16, [TypeCode]::UInt16,
            [TypeCode]::Int32, [TypeCode]::UInt32, [TypeCode]::Int64, [TypeCode]::UInt64,
            [TypeCode]::Single, [TypeCode]::Double, [TypeCode]::Decimal))
        {
            throw 'unsupported numeric type'
        }
        $number = [decimal]$Value
        if ($number -lt 0 -or $number -gt [decimal][UInt32]::MaxValue -or [decimal]::Truncate($number) -ne $number)
        {
            throw 'out of range or fractional'
        }
        return [UInt32]$number
    }
    catch
    {
        throw "$Name must be a non-negative UInt32 integer"
    }
}

function Read-PBBoundedJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][UInt64]$MaximumBytes
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
    {
        throw "JSON artifact does not exist: $resolvedPath"
    }
    $item = Get-Item -LiteralPath $resolvedPath
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        [UInt64]$item.Length -eq 0 -or [UInt64]$item.Length -gt $MaximumBytes)
    {
        throw "JSON artifact is empty, oversized, or a reparse point: $resolvedPath"
    }
    try
    {
        $text = Get-Content -LiteralPath $resolvedPath -Raw -Encoding UTF8
        return ConvertFrom-PBStrictJsonText -Text $text -Name "JSON artifact $resolvedPath"
    }
    catch
    {
        throw "JSON artifact is malformed: $resolvedPath ($($_.Exception.Message))"
    }
}

function Get-PBFileIdentity
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$RelativeTo = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
    {
        throw "Required regular file does not exist: $resolvedPath"
    }
    $item = Get-Item -LiteralPath $resolvedPath
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Required regular file must not be a reparse point: $resolvedPath"
    }
    $reportedPath = $resolvedPath
    if (-not [string]::IsNullOrWhiteSpace($RelativeTo))
    {
        $resolvedRoot = [System.IO.Path]::GetFullPath($RelativeTo).TrimEnd('\') + '\'
        if (-not $resolvedPath.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "File is outside the requested identity root: $resolvedPath"
        }
        $reportedPath = $resolvedPath.Substring($resolvedRoot.Length).Replace('\', '/')
    }
    return [ordered]@{
        path = $reportedPath
        size = [UInt64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Assert-PBIdentityShape
{
    param(
        [Parameter(Mandatory = $true)][object]$Identity,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Identity -isnot [System.Collections.IDictionary] -or
        [string]::IsNullOrWhiteSpace([string]$Identity.path) -or
        $Identity.size -is [bool] -or [UInt64]$Identity.size -eq 0 -or
        [string]$Identity.sha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "$Name does not contain a valid path/size/SHA-256 identity"
    }
}

function Assert-PBFileIdentity
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$IgnoreExpectedPath
    )
    Assert-PBIdentityShape -Identity $Expected -Name $Name
    $actual = Get-PBFileIdentity -Path $Path
    if ([UInt64]$actual.size -ne [UInt64]$Expected.size -or [string]$actual.sha256 -cne [string]$Expected.sha256 -or
        (-not $IgnoreExpectedPath -and [string]$actual.path -cne [System.IO.Path]::GetFullPath([string]$Expected.path)))
    {
        throw "$Name exact identity mismatch"
    }
    return $actual
}

function Write-PBCreateOnlyJson
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value,
        [ValidateRange(2, 100)][int]$Depth = 20
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $temporaryPath = "$resolvedPath.partial"
    if ((Test-Path -LiteralPath $resolvedPath) -or (Test-Path -LiteralPath $temporaryPath))
    {
        throw "Create-only JSON output or partial already exists: $resolvedPath"
    }
    $parent = [System.IO.Path]::GetDirectoryName($resolvedPath)
    if ([string]::IsNullOrWhiteSpace($parent) -or -not (Test-Path -LiteralPath $parent -PathType Container))
    {
        throw "Create-only JSON parent directory does not exist: $parent"
    }
    $parentItem = Get-Item -LiteralPath $parent
    if ($parentItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
    {
        throw "Create-only JSON parent must not be a reparse point: $parent"
    }
    try
    {
        $stream = [System.IO.File]::Open($temporaryPath, [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        try
        {
            $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
            try
            {
                $writer.Write(($Value | ConvertTo-Json -Depth $Depth))
                $writer.Flush()
                $stream.Flush($true)
            }
            finally
            {
                $writer.Dispose()
            }
        }
        finally
        {
            $stream.Dispose()
        }
        Move-Item -LiteralPath $temporaryPath -Destination $resolvedPath
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
        {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
    return Get-PBFileIdentity -Path $resolvedPath
}

function Get-PBRectDimensions
{
    param(
        [Parameter(Mandatory = $true)][object]$Rect,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Rect -isnot [System.Collections.IDictionary])
    {
        throw "$Name must be a JSON object"
    }
    foreach ($field in @('left', 'top', 'right', 'bottom'))
    {
        if (-not $Rect.Contains($field) -or $Rect[$field] -is [bool] -or
            $Rect[$field] -isnot [byte] -and $Rect[$field] -isnot [sbyte] -and
            $Rect[$field] -isnot [int16] -and $Rect[$field] -isnot [uint16] -and
            $Rect[$field] -isnot [int32] -and $Rect[$field] -isnot [uint32] -and
            $Rect[$field] -isnot [int64] -and $Rect[$field] -isnot [uint64])
        {
            throw "$Name.$field must be an integer"
        }
    }
    $left = [Int64]$Rect.left
    $top = [Int64]$Rect.top
    $right = [Int64]$Rect.right
    $bottom = [Int64]$Rect.bottom
    $width = $right - $left
    $height = $bottom - $top
    if ($width -le 0 -or $height -le 0 -or $width -gt 8192 -or $height -gt 8192 -or $width * $height -gt 8MB)
    {
        throw "$Name has invalid or unbounded dimensions"
    }
    return [ordered]@{ left = $left; top = $top; right = $right; bottom = $bottom; width = $width; height = $height }
}

function Test-PBRectContains
{
    param(
        [Parameter(Mandatory = $true)][object]$Outer,
        [Parameter(Mandatory = $true)][object]$Inner
    )
    return [Int64]$Inner.left -ge [Int64]$Outer.left -and [Int64]$Inner.top -ge [Int64]$Outer.top -and
        [Int64]$Inner.right -le [Int64]$Outer.right -and [Int64]$Inner.bottom -le [Int64]$Outer.bottom
}

function Test-PBRectsOverlap
{
    param(
        [Parameter(Mandatory = $true)][object]$First,
        [Parameter(Mandatory = $true)][object]$Second
    )
    return [Int64]$First.left -lt [Int64]$Second.right -and [Int64]$First.right -gt [Int64]$Second.left -and
        [Int64]$First.top -lt [Int64]$Second.bottom -and [Int64]$First.bottom -gt [Int64]$Second.top
}

function Get-PBPostDecoderBroadcastProofWindow
{
    param(
        [Parameter(Mandatory = $true)][Int64]$DecoderEvidenceReadyUnixMilliseconds,
        [Parameter(Mandatory = $true)][Int32]$DecoderClockOffsetMilliseconds,
        [Parameter(Mandatory = $true)][ValidateRange(1, 600)][UInt32]$ProofSeconds
    )
    if ($DecoderEvidenceReadyUnixMilliseconds -le 0)
    {
        throw 'Decoder evidence-ready timestamp must be positive'
    }
    $adjustedDecimal = [decimal]$DecoderEvidenceReadyUnixMilliseconds + [decimal]$DecoderClockOffsetMilliseconds
    $requiredDecimal = [decimal]$ProofSeconds * 1000
    $thresholdDecimal = $adjustedDecimal + $requiredDecimal
    if ($adjustedDecimal -le 0 -or $adjustedDecimal -gt [Int64]::MaxValue -or
        $thresholdDecimal -le 0 -or $thresholdDecimal -gt [Int64]::MaxValue)
    {
        throw 'Post-Decoder broadcast proof window overflows the supported timestamp domain'
    }
    return [ordered]@{
        adjustedDecoderEvidenceReadyUnixMilliseconds = [Int64]$adjustedDecimal
        requiredProofMilliseconds = [Int64]$requiredDecimal
        thresholdUnixMilliseconds = [Int64]$thresholdDecimal
    }
}

function Assert-PBMonitorRuntimeContract
{
    param(
        [Parameter(Mandatory = $true)][object]$Contract,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($Contract -isnot [System.Collections.IDictionary] -or
        [string]::IsNullOrWhiteSpace([string]$Contract.deviceName) -or
        [UInt32]$Contract.dpiX -eq 0 -or [UInt32]$Contract.dpiY -eq 0 -or
        [UInt32]$Contract.refreshRate -eq 0 -or [string]$Contract.rotation -cne 'Identity' -or
        $Contract.primary -isnot [bool] -or $Contract.adapterLuid -isnot [System.Collections.IDictionary])
    {
        throw "$Name monitor runtime contract is incomplete or invalid"
    }
    [void](Get-PBRectDimensions -Rect $Contract.physicalRect -Name "$Name physicalRect")
    foreach ($field in @('high', 'low'))
    {
        if (-not $Contract.adapterLuid.Contains($field) -or $Contract.adapterLuid[$field] -is [bool] -or
            $Contract.adapterLuid[$field] -isnot [byte] -and $Contract.adapterLuid[$field] -isnot [sbyte] -and
            $Contract.adapterLuid[$field] -isnot [int16] -and $Contract.adapterLuid[$field] -isnot [uint16] -and
            $Contract.adapterLuid[$field] -isnot [int32] -and $Contract.adapterLuid[$field] -isnot [uint32] -and
            $Contract.adapterLuid[$field] -isnot [int64] -and $Contract.adapterLuid[$field] -isnot [uint64])
        {
            throw "$Name adapterLuid.$field must be an integer"
        }
    }
}

function Test-PBMonitorRuntimeContractMatch
{
    param(
        [Parameter(Mandatory = $true)][object]$Actual,
        [Parameter(Mandatory = $true)][object]$Expected
    )
    foreach ($field in @('left', 'top', 'right', 'bottom'))
    {
        if ([Int64]$Actual.physicalRect[$field] -ne [Int64]$Expected.physicalRect[$field])
        {
            return $false
        }
    }
    return [string]$Actual.deviceName -ceq [string]$Expected.deviceName -and
        [UInt32]$Actual.dpiX -eq [UInt32]$Expected.dpiX -and [UInt32]$Actual.dpiY -eq [UInt32]$Expected.dpiY -and
        [UInt32]$Actual.refreshRate -eq [UInt32]$Expected.refreshRate -and
        [string]$Actual.rotation -ceq [string]$Expected.rotation -and
        [Int64]$Actual.adapterLuid.high -eq [Int64]$Expected.adapterLuid.high -and
        [UInt64]$Actual.adapterLuid.low -eq [UInt64]$Expected.adapterLuid.low -and
        [bool]$Actual.primary -eq [bool]$Expected.primary
}

function New-PBRemoteVisualMonitorPreflight
{
    param(
        [Parameter(Mandatory = $true)][string]$DecoderPath,
        [Parameter(Mandatory = $true)][object]$Safety,
        [Parameter(Mandatory = $true)][object]$TargetRect,
        [Parameter(Mandatory = $true)][ValidateSet('Encoder', 'DecoderLive')][string]$EndpointRole,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )
    Assert-PBMonitorRuntimeContract -Contract $Safety.protectedMonitorContract -Name "$EndpointRole ProtectedMonitor"
    Assert-PBMonitorRuntimeContract -Contract $Safety.experimentMonitorContract -Name "$EndpointRole ExperimentMonitor"
    [void](Get-PBRectDimensions -Rect $TargetRect -Name "$EndpointRole targetRect")
    $resolvedDecoder = [System.IO.Path]::GetFullPath($DecoderPath)
    if (-not (Test-Path -LiteralPath $resolvedDecoder -PathType Leaf) -or
        (Get-Item -LiteralPath $resolvedDecoder).Attributes -band [System.IO.FileAttributes]::ReparsePoint)
    {
        throw "$EndpointRole monitor preflight Decoder is unavailable or a reparse point"
    }
    $catalogOutput = @(& $resolvedDecoder --list-monitors 2>&1)
    if ($LASTEXITCODE -ne 0)
    {
        throw "$EndpointRole monitor preflight failed to enumerate the packaged runtime catalog"
    }
    $catalogText = $catalogOutput -join "`n"
    if ([System.Text.UTF8Encoding]::new($false, $true).GetByteCount($catalogText) -gt 1MB)
    {
        throw "$EndpointRole monitor preflight catalog exceeds 1 MiB"
    }
    $catalog = ConvertFrom-PBStrictJsonText -Text $catalogText -Name "$EndpointRole live monitor catalog"
    if ([string]$catalog.schema -cne 'PixelBridge.MonitorCatalog.1' -or
        [UInt32]$catalog.monitorCount -ne @($catalog.monitors).Count -or @($catalog.monitors).Count -gt 64)
    {
        throw "$EndpointRole live monitor catalog schema/count is invalid"
    }
    $protectedMatches = @($catalog.monitors | Where-Object {
        [string]$_.deviceName -ceq [string]$Safety.protectedMonitorContract.deviceName
    })
    $experimentMatches = @($catalog.monitors | Where-Object {
        [string]$_.deviceName -ceq [string]$Safety.experimentMonitorContract.deviceName
    })
    if ($protectedMatches.Count -ne 1 -or $experimentMatches.Count -ne 1 -or
        -not (Test-PBMonitorRuntimeContractMatch -Actual $protectedMatches[0] -Expected $Safety.protectedMonitorContract) -or
        -not (Test-PBMonitorRuntimeContractMatch -Actual $experimentMatches[0] -Expected $Safety.experimentMonitorContract) -or
        (Test-PBRectsOverlap -First $protectedMatches[0].physicalRect -Second $experimentMatches[0].physicalRect) -or
        -not (Test-PBRectContains -Outer $experimentMatches[0].physicalRect -Inner $TargetRect) -or
        (Test-PBRectsOverlap -First $protectedMatches[0].physicalRect -Second $TargetRect))
    {
        throw "$EndpointRole live monitor catalog differs from the frozen target/safety contract"
    }
    $preflight = [ordered]@{
        schema = 'PixelBridge.RemoteVisualMonitorPreflight.1'
        capturedUtc = [DateTime]::UtcNow.ToString('o')
        endpointRole = $EndpointRole
        status = 'PASS'
        collectionSemantics = 'Packaged PixelBridgeDecoder --list-monitors; no input, focus, window, or display mutation'
        decoder = Get-PBFileIdentity -Path $resolvedDecoder
        targetRect = $TargetRect
        protectedMonitor = $protectedMatches[0]
        experimentMonitor = $experimentMatches[0]
        catalog = $catalog
    }
    return Write-PBCreateOnlyJson -Path $OutputPath -Value $preflight -Depth 30
}

function Import-PBRemoteVisualPilotPlan
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256) -and
        ($ExpectedSha256 -cnotmatch '^[0-9a-f]{64}$' -or [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'RemoteVisual pilot plan SHA-256 differs from the expected frozen identity'
    }
    if ([UInt64]$identity.size -gt 256KB)
    {
        throw 'RemoteVisual pilot plan exceeds 256 KiB'
    }
    $plan = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 256KB
    $schema = [string]$plan.schema
    if (@('PixelBridge.RemoteVisualPilotPlan.1', 'PixelBridge.RemoteVisualPilotPlan.2',
        'PixelBridge.RemoteVisualPilotPlan.3') -cnotcontains $schema -or
        [string]$plan.runId -cnotmatch '^[0-9a-f]{32}$')
    {
        throw 'RemoteVisual pilot plan schema or RunId is invalid'
    }
    $profileContract = switch ([string]$plan.profileToken)
    {
        'direct'
        {
            [ordered]@{
                name = 'Direct-Level 2x2 (Experimental)'
                visualProfileId = [UInt64]::Parse('EBB15DCE41AB436E', [Globalization.NumberStyles]::HexNumber)
                visualLayoutVersion = 3
                codedDataBytesPerFrame = 86688
                codewordsPerFrame = 42
                scaledGeometry = $false
            }
        }
        'shape'
        {
            [ordered]@{
                name = 'Shape+Chroma (Experimental)'
                visualProfileId = [UInt64]::Parse('5042534843503031', [Globalization.NumberStyles]::HexNumber)
                visualLayoutVersion = 4
                codedDataBytesPerFrame = 65016
                codewordsPerFrame = 32
                scaledGeometry = $false
            }
        }
        'remote-lf4'
        {
            [ordered]@{
                name = 'PB-RemoteVisual-LF4-X1 (Experimental)'
                visualProfileId = [UInt64]::Parse('504252564C463431', [Globalization.NumberStyles]::HexNumber)
                visualLayoutVersion = 7
                codedDataBytesPerFrame = 8100
                codewordsPerFrame = 4
                scaledGeometry = $true
            }
        }
        default { $null }
    }
    if ($null -eq $profileContract -or
        ($schema -ceq 'PixelBridge.RemoteVisualPilotPlan.1' -and [string]$plan.profileToken -cne 'remote-lf4') -or
        [string]$plan.profileName -cne [string]$profileContract.name -or
        [UInt64]$plan.visualProfileId -ne [UInt64]$profileContract.visualProfileId -or
        [UInt32]$plan.visualLayoutVersion -ne [UInt32]$profileContract.visualLayoutVersion -or
        [UInt32]$plan.codedDataBytesPerFrame -ne [UInt32]$profileContract.codedDataBytesPerFrame -or
        [UInt32]$plan.codewordsPerFrame -ne [UInt32]$profileContract.codewordsPerFrame)
    {
        throw 'RemoteVisual pilot plan identity/profile constants mismatch'
    }
    $isStep21Plan = $schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2' -or
        $schema -ceq 'PixelBridge.RemoteVisualPilotPlan.3'
    $isCurrentStep21Plan = $schema -ceq 'PixelBridge.RemoteVisualPilotPlan.3'
    if ($isStep21Plan)
    {
        if ($plan.matrix -isnot [System.Collections.IDictionary] -or
            [string]$plan.matrix.modeClass -notin @('QualityPriority', 'Automatic', 'Restricted') -or
            [string]::IsNullOrWhiteSpace([string]$plan.matrix.visibleRemoteMode) -or
            [string]$plan.matrix.profileComparisonRole -cne $(if ([string]$plan.profileToken -ceq 'remote-lf4') { 'Candidate' } else { 'Baseline' }) -or
            [string]$plan.matrix.backendCoverageRole -cne $(if ([string]$plan.policy.captureBackend -ceq 'wgc') { 'MainMatrix' } else { 'RepresentativeRecheck' }) -or
            [string]$plan.matrix.runIsolation -cne 'IndependentRunIdAndArtifacts; metrics from different runs must not be merged')
        {
            throw 'RemoteVisual Step 21 plan matrix identity or run-isolation contract mismatch'
        }
        if ($isCurrentStep21Plan)
        {
            Assert-PBPilotExactKeys -Dictionary $plan.matrix -ExpectedKeys @('modeClass', 'visibleRemoteMode',
                'scaleTarget', 'profileComparisonRole', 'backendCoverageRole', 'runIsolation') `
                -Name 'RemoteVisual Step 21 matrix tuple'
        }
        if ($isCurrentStep21Plan -and
            ([string]$plan.matrix.scaleTarget -notin @('0.750', '1.000', '1.259', '1.500') -or
             [string]$plan.matrix.scaleTarget -cne [string]$plan.remoteGeometry.expectedLocatorScaleTarget))
        {
            throw 'RemoteVisual Step 21 plan lacks one explicit matching Locator scale target'
        }
    }
    $logicalFps = [UInt32]$plan.logicalFps
    if ($logicalFps -notin @(1, 2, 5))
    {
        throw 'RemoteVisual pilot plan logicalFps must be exactly 1, 2, or 5'
    }
    if ([string]$plan.geometryMode -notin @('Strict1To1', 'LocatorScaled'))
    {
        throw 'RemoteVisual pilot plan geometryMode is invalid'
    }
    Assert-PBIdentityShape -Identity $plan.deployment.manifest -Name 'deployment manifest'
    Assert-PBIdentityShape -Identity $plan.deployment.packageManifest -Name 'package manifest'
    Assert-PBIdentityShape -Identity $plan.deployment.sourceManifest -Name 'source manifest'
    Assert-PBIdentityShape -Identity $plan.deployment.remoteMetadata -Name 'remote metadata'
    Assert-PBIdentityShape -Identity $plan.deployment.encoderEnvironment -Name 'Encoder environment'
    Assert-PBIdentityShape -Identity $plan.deployment.decoderEnvironment -Name 'Decoder environment'
    Assert-PBIdentityShape -Identity $plan.remoteUi.evidence -Name 'remote UI evidence'
    Assert-PBIdentityShape -Identity $plan.remoteUi.screenshot -Name 'remote UI screenshot'
    Assert-PBIdentityShape -Identity $plan.remoteUi.captureRecord -Name 'remote UI capture record'
    foreach ($role in @('encoder', 'decoder'))
    {
        $application = $plan.applications[$role]
        $roleName = if ($role -ceq 'encoder') { 'Encoder' } else { 'Decoder' }
        if ([string]$application.role -cne $roleName -or
            [string]$application.relativeExecutablePath -cne "$roleName/PixelBridge$roleName.exe" -or
            [UInt64]$application.size -eq 0 -or [string]$application.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            [string]::IsNullOrWhiteSpace([string]$application.versionOutput))
        {
            throw "RemoteVisual pilot plan $roleName application identity is invalid"
        }
    }
    if ([string]$plan.deployment.runId -cne [string]$plan.runId -or
        [string]$plan.remoteUi.runId -cne [string]$plan.runId -or
        [string]$plan.source.relativePath -cne 'random-1MiB.bin' -or
        [UInt64]$plan.source.size -ne 1MB -or [string]$plan.source.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        [string]$plan.source.pixelBridgeSegmentCompression -cne 'RAW/OFF')
    {
        throw 'RemoteVisual pilot plan RunId/source contract mismatch'
    }
    if ($isStep21Plan -and
        ([string]$plan.remoteUi.provenance -cne 'RemoteUiVisible' -or
         [string]$plan.remoteUi.visibleClaims.remoteMode -cne [string]$plan.matrix.visibleRemoteMode -or
         @($plan.remoteUi.visibleFields) -cnotcontains 'remoteMode'))
    {
        throw 'RemoteVisual Step 21 plan mode class lacks matching visible UI evidence'
    }

    $encoderSafety = $plan.monitorSafety.encoder
    $decoderSafety = $plan.monitorSafety.decoder
    if ([string]::IsNullOrWhiteSpace([string]$encoderSafety.protectedMonitorDeviceName) -or
        [string]::IsNullOrWhiteSpace([string]$encoderSafety.experimentMonitorDeviceName) -or
        [string]$encoderSafety.protectedMonitorDeviceName -ieq [string]$encoderSafety.experimentMonitorDeviceName -or
        [string]::IsNullOrWhiteSpace([string]$decoderSafety.protectedMonitorDeviceName) -or
        [string]::IsNullOrWhiteSpace([string]$decoderSafety.experimentMonitorDeviceName) -or
        [string]$decoderSafety.protectedMonitorDeviceName -ieq [string]$decoderSafety.experimentMonitorDeviceName)
    {
        throw 'RemoteVisual pilot plan requires distinct explicit protected/experiment monitors at both endpoints'
    }
    $dataWindow = Get-PBRectDimensions -Rect $encoderSafety.dataWindowPhysicalRect -Name 'Encoder Data Window'
    $decoderRoi = Get-PBRectDimensions -Rect $decoderSafety.roiPhysicalRect -Name 'Decoder ROI'
    if ($dataWindow.width -ne 1920 -or $dataWindow.height -ne 1080)
    {
        throw 'RemoteVisual LF4 Encoder Data Window must be exactly 1920x1080 physical pixels'
    }
    $captureRoiScaleX = [double]$decoderRoi.width / 1920.0
    $captureRoiScaleY = [double]$decoderRoi.height / 1080.0
    if ($isCurrentStep21Plan)
    {
        Assert-PBPilotExactKeys -Dictionary $plan.remoteGeometry -ExpectedKeys @('captureRoiWidth',
            'captureRoiHeight', 'captureRoiScaleX', 'captureRoiScaleY', 'expectedLocatorScaleTarget',
            'scaleAuthority', 'axisAligned', 'cropStatus', 'locatorRemainsAuthoritative') `
            -Name 'RemoteVisual Step 21 geometry contract'
        $recordedCaptureRoiWidth = Get-PBPilotUInt32 -Value $plan.remoteGeometry.captureRoiWidth `
            -Name 'RemoteVisual Step 21 captureRoiWidth'
        $recordedCaptureRoiHeight = Get-PBPilotUInt32 -Value $plan.remoteGeometry.captureRoiHeight `
            -Name 'RemoteVisual Step 21 captureRoiHeight'
        $recordedCaptureRoiScaleX = Get-PBPilotFiniteDouble -Value $plan.remoteGeometry.captureRoiScaleX `
            -Name 'RemoteVisual Step 21 captureRoiScaleX'
        $recordedCaptureRoiScaleY = Get-PBPilotFiniteDouble -Value $plan.remoteGeometry.captureRoiScaleY `
            -Name 'RemoteVisual Step 21 captureRoiScaleY'
        if ($recordedCaptureRoiWidth -ne [UInt32]$decoderRoi.width -or
            $recordedCaptureRoiHeight -ne [UInt32]$decoderRoi.height -or
            [Math]::Abs($recordedCaptureRoiScaleX - $captureRoiScaleX) -gt 0.0000001 -or
            [Math]::Abs($recordedCaptureRoiScaleY - $captureRoiScaleY) -gt 0.0000001 -or
            [string]$plan.remoteGeometry.scaleAuthority -cne 'AcceptedBootstrapLocatorPixels' -or
            $plan.remoteGeometry.axisAligned -isnot [bool] -or -not [bool]$plan.remoteGeometry.axisAligned -or
            [string]$plan.remoteGeometry.cropStatus -cne 'NoneExpected' -or
            $plan.remoteGeometry.locatorRemainsAuthoritative -isnot [bool] -or
            -not [bool]$plan.remoteGeometry.locatorRemainsAuthoritative)
        {
            throw 'RemoteVisual Step 21 capture ROI or Locator authority contract is invalid'
        }
        $scaleTarget = [string]$plan.matrix.scaleTarget
        $requiredTargetDimensions = switch ($scaleTarget)
        {
            '0.750' { [ordered]@{ width = 1440; height = 810 } }
            '1.000' { [ordered]@{ width = 1920; height = 1080 } }
            '1.259' { [ordered]@{ width = 2417; height = 1360 } }
            '1.500' { [ordered]@{ width = 2880; height = 1620 } }
        }
        if ([bool]$profileContract.scaledGeometry)
        {
            $expectedGeometryMode = if ($scaleTarget -ceq '1.000') { 'Strict1To1' } else { 'LocatorScaled' }
            if ($decoderRoi.width -lt 960 -or $decoderRoi.width -gt 3840 -or
                $decoderRoi.height -lt 540 -or $decoderRoi.height -gt 2160 -or
                $decoderRoi.width -lt [int]$requiredTargetDimensions.width -or
                $decoderRoi.height -lt [int]$requiredTargetDimensions.height -or
                [string]$plan.geometryMode -cne $expectedGeometryMode)
            {
                throw 'RemoteVisual LF4 Step 21 ROI cannot contain its target or has the wrong target geometry mode'
            }
        }
        elseif ($scaleTarget -cne '1.000' -or [string]$plan.geometryMode -cne 'Strict1To1' -or
            $decoderRoi.width -ne 1920 -or $decoderRoi.height -ne 1080)
        {
            throw 'Direct and Shape Step 21 plans require target 1.000 and an exact strict 1920x1080 ROI'
        }
    }
    else
    {
        $estimatedScaleX = Get-PBPilotFiniteDouble -Value $plan.remoteGeometry.estimatedScaleX `
            -Name 'RemoteVisual pilot estimatedScaleX'
        $estimatedScaleY = Get-PBPilotFiniteDouble -Value $plan.remoteGeometry.estimatedScaleY `
            -Name 'RemoteVisual pilot estimatedScaleY'
        if ([Math]::Abs($estimatedScaleX - $captureRoiScaleX) -gt 0.0000001 -or
            [Math]::Abs($estimatedScaleY - $captureRoiScaleY) -gt 0.0000001)
        {
            throw 'RemoteVisual pilot ROI scale is inconsistent with its physical rectangle'
        }
        if ([bool]$profileContract.scaledGeometry -and
            ($captureRoiScaleX -lt 0.5 -or $captureRoiScaleX -gt 2.0 -or
             $captureRoiScaleY -lt 0.5 -or $captureRoiScaleY -gt 2.0))
        {
            throw 'RemoteVisual LF4 pilot ROI is outside the 0.5..2.0 domain'
        }
        if (-not [bool]$profileContract.scaledGeometry -and
            ([string]$plan.geometryMode -cne 'Strict1To1' -or
             [Math]::Abs($captureRoiScaleX - 1.0) -gt 0.0000001 -or
             [Math]::Abs($captureRoiScaleY - 1.0) -gt 0.0000001))
        {
            throw 'Direct and Shape field-run plans require strict 1:1 geometry; no silent resampling is permitted'
        }
        if ([string]$plan.geometryMode -ceq 'Strict1To1' -and
            ([Math]::Abs($captureRoiScaleX - 1.0) -gt 0.0000001 -or
             [Math]::Abs($captureRoiScaleY - 1.0) -gt 0.0000001))
        {
            throw 'Strict1To1 pilot plan must use an exact 1920x1080 Decoder ROI'
        }
        if ([string]$plan.geometryMode -ceq 'LocatorScaled' -and
            [Math]::Abs($captureRoiScaleX - 1.0) -le 0.0000001 -and
            [Math]::Abs($captureRoiScaleY - 1.0) -le 0.0000001)
        {
            throw 'LocatorScaled pilot plan must exercise a non-1:1 geometry'
        }
    }

    $expectedPolicy = switch ($logicalFps)
    {
        1 { [ordered]@{ encoderSeconds = 600; decoderSeconds = 540; noProgressSeconds = 120; replaySampleFps = 2 } }
        2 { [ordered]@{ encoderSeconds = 360; decoderSeconds = 300; noProgressSeconds = 90; replaySampleFps = 4 } }
        5 { [ordered]@{ encoderSeconds = 180; decoderSeconds = 120; noProgressSeconds = 60; replaySampleFps = 10 } }
    }
    if ($schema -ceq 'PixelBridge.RemoteVisualPilotPlan.2' -and
        [Math]::Max($captureRoiScaleX, $captureRoiScaleY) -gt 1.3)
    {
        $expectedPolicy.replaySampleFps = $logicalFps
    }
    elseif ($isCurrentStep21Plan)
    {
        $prospectiveFrames = [UInt64]$expectedPolicy.replaySampleFps * ([UInt64]$expectedPolicy.decoderSeconds + 2) + 2
        $prospectiveBytes = [UInt64]$decoderRoi.width * [UInt64]$decoderRoi.height * 4 * $prospectiveFrames + 256MB
        if ($prospectiveBytes -gt 16GB)
        {
            $expectedPolicy.replaySampleFps = $logicalFps
        }
    }
    $policy = $plan.policy
    $captureBackendValid = if ($schema -ceq 'PixelBridge.RemoteVisualPilotPlan.1')
    {
        [string]$policy.captureBackend -ceq 'wgc'
    }
    else
    {
        [string]$policy.captureBackend -in @('wgc', 'dxgi')
    }
    if (-not $captureBackendValid -or [string]$policy.compression -cne 'off' -or
        [UInt32]$policy.encoderHardMaximumSeconds -ne $expectedPolicy.encoderSeconds -or
        [UInt32]$policy.decoderTimeoutSeconds -ne $expectedPolicy.decoderSeconds -or
        [UInt32]$policy.noProgressSeconds -ne $expectedPolicy.noProgressSeconds -or
        [UInt32]$policy.offlineReplayTimeoutSeconds -ne 600 -or [UInt32]$policy.offlineNoProgressSeconds -ne 120 -or
        [UInt32]$policy.decoderWarmupMaximumSeconds -ne 30 -or [UInt32]$policy.controlRepetitions -ne 12 -or
        [UInt32]$policy.postPublishReplayTailSeconds -ne 2 -or
        [UInt32]$policy.postDecoderBroadcastProofSeconds -ne [UInt32]([Math]::Ceiling([UInt32]$policy.clock.uncertaintyMilliseconds / 1000.0) + 2) -or
        [UInt32]$policy.replay.maximumCaptureFramesPerSecond -ne $expectedPolicy.replaySampleFps -or
        [UInt32]$policy.replay.maximumMiB -ne 16384 -or
        [UInt32]$policy.replay.reservedNonFrameMiB -ne 256 -or
        $policy.manualStopRequired -isnot [bool] -or -not [bool]$policy.manualStopRequired -or
        $policy.decoderStartsBeforeEncoder -isnot [bool] -or -not [bool]$policy.decoderStartsBeforeEncoder -or
        $policy.noFileClipboardOrIpcSideChannel -isnot [bool] -or -not [bool]$policy.noFileClipboardOrIpcSideChannel)
    {
        throw 'RemoteVisual pilot plan does not use the frozen Step 20 execution policy'
    }
    if ($isStep21Plan)
    {
        $expectedEncoderRuntimeEnforcement = if ([string]$plan.profileToken -ceq 'remote-lf4')
        {
            'ProductionRuntimePreflightAndPeriodicRevalidation'
        }
        else
        {
            'PlanAndEndpointEnvironmentContainment; current Direct/Shape Encoder CLI does not accept monitor-safety arguments'
        }
        if ([string]$encoderSafety.runtimeEnforcement -cne $expectedEncoderRuntimeEnforcement -or
            [string]$decoderSafety.runtimeEnforcement -cne 'ProductionRuntimePreflightAndPeriodicRevalidation')
        {
            throw 'RemoteVisual Step 21 plan monitor-safety enforcement boundary mismatch'
        }
        Assert-PBMonitorRuntimeContract -Contract $encoderSafety.protectedMonitorContract -Name 'Encoder ProtectedMonitor'
        Assert-PBMonitorRuntimeContract -Contract $encoderSafety.experimentMonitorContract -Name 'Encoder ExperimentMonitor'
        Assert-PBMonitorRuntimeContract -Contract $decoderSafety.protectedMonitorContract -Name 'Decoder ProtectedMonitor'
        Assert-PBMonitorRuntimeContract -Contract $decoderSafety.experimentMonitorContract -Name 'Decoder ExperimentMonitor'
        if ([string]$encoderSafety.protectedMonitorDeviceName -cne [string]$encoderSafety.protectedMonitorContract.deviceName -or
            [string]$encoderSafety.experimentMonitorDeviceName -cne [string]$encoderSafety.experimentMonitorContract.deviceName -or
            [string]$decoderSafety.protectedMonitorDeviceName -cne [string]$decoderSafety.protectedMonitorContract.deviceName -or
            [string]$decoderSafety.experimentMonitorDeviceName -cne [string]$decoderSafety.experimentMonitorContract.deviceName)
        {
            throw 'RemoteVisual Step 21 monitor runtime contracts disagree with selected device names'
        }
        $rectBindings = @(
            [ordered]@{ selected = $encoderSafety.protectedMonitorPhysicalRect; contract = $encoderSafety.protectedMonitorContract.physicalRect },
            [ordered]@{ selected = $encoderSafety.experimentMonitorPhysicalRect; contract = $encoderSafety.experimentMonitorContract.physicalRect },
            [ordered]@{ selected = $decoderSafety.protectedMonitorPhysicalRect; contract = $decoderSafety.protectedMonitorContract.physicalRect },
            [ordered]@{ selected = $decoderSafety.experimentMonitorPhysicalRect; contract = $decoderSafety.experimentMonitorContract.physicalRect })
        foreach ($binding in $rectBindings)
        {
            foreach ($field in @('left', 'top', 'right', 'bottom'))
            {
                if ([Int64]$binding.selected[$field] -ne [Int64]$binding.contract[$field])
                {
                    throw 'RemoteVisual Step 21 monitor runtime contract rectangle mismatch'
                }
            }
        }
    }
    if ([Int32]$policy.clock.decoderOffsetMilliseconds -lt -300000 -or [Int32]$policy.clock.decoderOffsetMilliseconds -gt 300000 -or
        [UInt32]$policy.clock.uncertaintyMilliseconds -gt 300000)
    {
        throw 'RemoteVisual pilot plan clock offset/uncertainty exceeds the bounded evidence contract'
    }
    $worstCaseFrames = [UInt64]$expectedPolicy.replaySampleFps * ([UInt64]$expectedPolicy.decoderSeconds + 2) + 2
    $expectedMaximumFrames = if ($schema -ceq 'PixelBridge.RemoteVisualPilotPlan.1') { [UInt32]1250 } else { [UInt32]$worstCaseFrames }
    if ($worstCaseFrames -gt 1250 -or [UInt32]$policy.replay.maximumFrames -ne $expectedMaximumFrames -or
        [UInt64]$policy.replay.worstCaseSampledFrames -ne $worstCaseFrames)
    {
        throw 'RemoteVisual pilot replay frame budget is inconsistent'
    }
    $worstCaseBytes = [UInt64]$decoderRoi.width * [UInt64]$decoderRoi.height * 4 * $expectedMaximumFrames + 256MB
    if ($worstCaseBytes -gt 16GB -or [UInt64]$policy.replay.worstCaseBytesIncludingReserve -ne $worstCaseBytes)
    {
        throw 'RemoteVisual pilot replay byte budget exceeds or disagrees with the frozen 16 GiB cap'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $plan }
}

Export-ModuleMember -Function ConvertFrom-PBStrictJsonText, Read-PBBoundedJson, Get-PBFileIdentity, Assert-PBIdentityShape, Assert-PBFileIdentity, Write-PBCreateOnlyJson, Get-PBRectDimensions, Test-PBRectContains, Test-PBRectsOverlap, Get-PBPostDecoderBroadcastProofWindow, New-PBRemoteVisualMonitorPreflight, Import-PBRemoteVisualPilotPlan

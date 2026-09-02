#Requires -Version 7.0

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pilotModule = Join-Path $PSScriptRoot 'PBRemoteVisualPilotCommon.psm1'
Import-Module -Name $pilotModule -ErrorAction Stop

function Assert-PBMatrixExactKeys
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
    $actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($key in $Dictionary.Keys)
    {
        if (-not $actual.Add([string]$key))
        {
            throw "$Name contains duplicate key '$key'"
        }
    }
    if ($actual.Count -ne $ExpectedKeys.Count)
    {
        throw "$Name does not contain the exact required key set"
    }
    foreach ($key in $ExpectedKeys)
    {
        if (-not $actual.Remove($key))
        {
            throw "$Name is missing exact key '$key'"
        }
    }
}

function Assert-PBMatrixIdentityEqual
{
    param(
        [Parameter(Mandatory = $true)][object]$Actual,
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )
    Assert-PBIdentityShape -Identity $Actual -Name $Name
    Assert-PBIdentityShape -Identity $Expected -Name "$Name expected"
    if ([UInt64]$Actual.size -ne [UInt64]$Expected.size -or [string]$Actual.sha256 -cne [string]$Expected.sha256)
    {
        throw "$Name identity mismatch"
    }
}

function Assert-PBMatrixBoundedIdentity
{
    param(
        [Parameter(Mandatory = $true)][object]$Identity,
        [Parameter(Mandatory = $true)][UInt64]$MaximumBytes,
        [Parameter(Mandatory = $true)][string]$Name
    )
    Assert-PBMatrixExactKeys -Dictionary $Identity -ExpectedKeys @('path', 'size', 'sha256') -Name $Name
    Assert-PBIdentityShape -Identity $Identity -Name $Name
    $size = Get-PBNonNegativeUInt64 -Value $Identity.size -Name "$Name size"
    if ($size -eq 0 -or $size -gt $MaximumBytes)
    {
        throw "$Name exceeds its bounded artifact size"
    }
}

function Get-PBStep21ScaleTarget
{
    param(
        [Parameter(Mandatory = $true)][double]$ScaleX,
        [Parameter(Mandatory = $true)][double]$ScaleY,
        [Parameter(Mandatory = $true)][string]$ProfileToken
    )
    if ([double]::IsNaN($ScaleX) -or [double]::IsInfinity($ScaleX) -or
        [double]::IsNaN($ScaleY) -or [double]::IsInfinity($ScaleY) -or
        $ScaleX -le 0.0 -or $ScaleY -le 0.0)
    {
        throw 'Step 21 scale must be finite and positive'
    }
    if ([Math]::Abs($ScaleX - $ScaleY) -gt 0.015)
    {
        throw 'Step 21 matrix rejects anisotropic geometry outside the frozen 0.015 scale tolerance'
    }
    if ($ProfileToken -in @('direct', 'shape'))
    {
        if ([Math]::Abs($ScaleX - 1.0) -gt 0.000001 -or [Math]::Abs($ScaleY - 1.0) -gt 0.000001)
        {
            throw 'Direct and Shape matrix records must remain exact 1:1 without resampling'
        }
        return '1.000'
    }
    if ($ProfileToken -cne 'remote-lf4')
    {
        throw 'Unknown Step 21 profile token'
    }
    $targets = @(
        [ordered]@{ label = '0.750'; value = 0.750 },
        [ordered]@{ label = '1.000'; value = 1.000 },
        [ordered]@{ label = '1.259'; value = 1.259 },
        [ordered]@{ label = '1.500'; value = 1.500 })
    $best = $null
    $bestDistance = [double]::PositiveInfinity
    foreach ($target in $targets)
    {
        $distance = [Math]::Max([Math]::Abs($ScaleX - [double]$target.value), [Math]::Abs($ScaleY - [double]$target.value))
        if ($distance -lt $bestDistance)
        {
            $best = $target
            $bestDistance = $distance
        }
    }
    if ($null -eq $best -or $bestDistance -gt 0.015)
    {
        throw 'LF4 geometry is outside the frozen approximately 0.75/1.0/1.259/1.5 Step 21 scale grid'
    }
    return [string]$best.label
}

function New-PBRemoteVisualStep21ExpectedCells
{
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('QualityPriority', 'Automatic', 'Restricted')]
        [string]$ComparisonModeClass,

        [Parameter(Mandatory = $true)]
        [ValidateSet(1, 2, 5)]
        [UInt32]$ComparisonLogicalFps
    )
    $modeSlugs = @{
        QualityPriority = 'quality-priority'
        Automatic = 'automatic'
        Restricted = 'restricted'
    }
    $scaleSlugs = @{
        '0.750' = '0750'
        '1.000' = '1000'
        '1.259' = '1259'
        '1.500' = '1500'
    }
    $cells = [Collections.Generic.List[object]]::new()
    foreach ($mode in @('QualityPriority', 'Automatic', 'Restricted'))
    {
        foreach ($scale in @('0.750', '1.000', '1.259', '1.500'))
        {
            foreach ($fps in @(1, 2, 5))
            {
                [void]$cells.Add([ordered]@{
                    cellId = "lf4-wgc-$($modeSlugs[$mode])-s$($scaleSlugs[$scale])-f$fps"
                    profileToken = 'remote-lf4'
                    captureBackend = 'wgc'
                    modeClass = $mode
                    scaleTarget = $scale
                    logicalFps = [UInt32]$fps
                    coverageRole = 'MainMatrix'
                    profileComparisonRole = 'Candidate'
                })
            }
        }
    }
    foreach ($case in @(
        [ordered]@{ mode = 'QualityPriority'; scale = '0.750'; fps = 1 },
        [ordered]@{ mode = 'Automatic'; scale = '1.259'; fps = 2 },
        [ordered]@{ mode = 'Restricted'; scale = '1.500'; fps = 5 }))
    {
        [void]$cells.Add([ordered]@{
            cellId = "lf4-dxgi-$($modeSlugs[$case.mode])-s$($scaleSlugs[$case.scale])-f$($case.fps)"
            profileToken = 'remote-lf4'
            captureBackend = 'dxgi'
            modeClass = [string]$case.mode
            scaleTarget = [string]$case.scale
            logicalFps = [UInt32]$case.fps
            coverageRole = 'RepresentativeRecheck'
            profileComparisonRole = 'Candidate'
        })
    }
    foreach ($profile in @('direct', 'shape'))
    {
        [void]$cells.Add([ordered]@{
            cellId = "$profile-wgc-$($modeSlugs[$ComparisonModeClass])-s1000-f$ComparisonLogicalFps"
            profileToken = $profile
            captureBackend = 'wgc'
            modeClass = $ComparisonModeClass
            scaleTarget = '1.000'
            logicalFps = $ComparisonLogicalFps
            coverageRole = 'ProfileBaseline'
            profileComparisonRole = 'Baseline'
        })
    }
    return @($cells)
}

function Get-PBRemoteVisualStep21CellRequiredRoi
{
    param([Parameter(Mandatory = $true)][object]$Cell)
    Assert-PBMatrixExactKeys -Dictionary $Cell -ExpectedKeys @('cellId', 'profileToken', 'captureBackend',
        'modeClass', 'scaleTarget', 'logicalFps', 'coverageRole', 'profileComparisonRole') -Name 'Step 21 matrix cell'
    if ([string]$Cell.profileToken -in @('direct', 'shape'))
    {
        if ([string]$Cell.scaleTarget -cne '1.000')
        {
            throw 'Direct and Shape Step 21 cells must remain exact 1:1'
        }
        return [ordered]@{ width = [UInt32]1920; height = [UInt32]1080 }
    }
    if ([string]$Cell.profileToken -cne 'remote-lf4')
    {
        throw 'Step 21 hardware scope contains an unknown profile token'
    }
    switch ([string]$Cell.scaleTarget)
    {
        '0.750' { return [ordered]@{ width = [UInt32]1440; height = [UInt32]810 } }
        '1.000' { return [ordered]@{ width = [UInt32]1920; height = [UInt32]1080 } }
        '1.259' { return [ordered]@{ width = [UInt32]2417; height = [UInt32]1360 } }
        '1.500' { return [ordered]@{ width = [UInt32]2880; height = [UInt32]1620 } }
        default { throw 'Step 21 hardware scope contains an unknown scale target' }
    }
}

function New-PBRemoteVisualStep21HardwareScopePartition
{
    param(
        [Parameter(Mandatory = $true)][object[]]$Cells,
        [Parameter(Mandatory = $true)][UInt32]$ExperimentMonitorWidth,
        [Parameter(Mandatory = $true)][UInt32]$ExperimentMonitorHeight
    )
    if ($ExperimentMonitorWidth -eq 0 -or $ExperimentMonitorHeight -eq 0)
    {
        throw 'Step 21 hardware scope monitor dimensions must be positive'
    }
    $included = [Collections.Generic.List[object]]::new()
    $excluded = [Collections.Generic.List[object]]::new()
    foreach ($cell in $Cells)
    {
        $requiredRoi = Get-PBRemoteVisualStep21CellRequiredRoi -Cell $cell
        if ([UInt32]$requiredRoi.width -le $ExperimentMonitorWidth -and [UInt32]$requiredRoi.height -le $ExperimentMonitorHeight)
        {
            [void]$included.Add($cell)
        }
        else
        {
            [void]$excluded.Add([ordered]@{
                cell = $cell
                requiredRoi = $requiredRoi
                exclusionReason = 'RequiredRoiExceedsSingleExperimentMonitor'
            })
        }
    }
    return [ordered]@{ includedCells = @($included); excludedCells = @($excluded) }
}

function Import-PBRemoteVisualStep21MatrixSpec
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 256KB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 21 matrix specification size or expected SHA-256 is invalid'
    }
    $spec = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 256KB
    Assert-PBMatrixExactKeys -Dictionary $spec -ExpectedKeys @('schema', 'createdUtc', 'matrixId',
        'comparisonModeClass', 'comparisonLogicalFps', 'cellCount', 'cells', 'contracts') -Name 'Step 21 matrix specification'
    $comparisonLogicalFps = Get-PBNonNegativeUInt64 -Value $spec.comparisonLogicalFps `
        -Name 'Step 21 matrix specification comparisonLogicalFps'
    $cellCount = Get-PBNonNegativeUInt64 -Value $spec.cellCount -Name 'Step 21 matrix specification cellCount'
    if ([string]$spec.schema -cne 'PixelBridge.RemoteVisualStep21MatrixSpec.1' -or
        [string]$spec.matrixId -cnotmatch '^[0-9a-f]{32}$' -or
        [string]$spec.comparisonModeClass -notin @('QualityPriority', 'Automatic', 'Restricted') -or
        $comparisonLogicalFps -notin @(1, 2, 5) -or $cellCount -ne 41 -or @($spec.cells).Count -ne 41)
    {
        throw 'Step 21 matrix specification identity or cell count is invalid'
    }
    Assert-PBMatrixExactKeys -Dictionary $spec.contracts -ExpectedKeys @('oneIndependentRunPerCell',
        'failedRunsAreRetained', 'differentRunMetricsMustNotBeMerged', 'uiVisibleModeRequired',
        'unknownChromaAndLatencyRemainUnknown', 'invalidGeometryNeverResampled', 'certifiedRemoteVisualProfile') `
        -Name 'Step 21 matrix specification contracts'
    if ($spec.contracts.oneIndependentRunPerCell -isnot [bool] -or -not [bool]$spec.contracts.oneIndependentRunPerCell -or
        $spec.contracts.failedRunsAreRetained -isnot [bool] -or -not [bool]$spec.contracts.failedRunsAreRetained -or
        $spec.contracts.differentRunMetricsMustNotBeMerged -isnot [bool] -or -not [bool]$spec.contracts.differentRunMetricsMustNotBeMerged -or
        $spec.contracts.uiVisibleModeRequired -isnot [bool] -or -not [bool]$spec.contracts.uiVisibleModeRequired -or
        $spec.contracts.unknownChromaAndLatencyRemainUnknown -isnot [bool] -or -not [bool]$spec.contracts.unknownChromaAndLatencyRemainUnknown -or
        $spec.contracts.invalidGeometryNeverResampled -isnot [bool] -or -not [bool]$spec.contracts.invalidGeometryNeverResampled -or
        $spec.contracts.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$spec.contracts.certifiedRemoteVisualProfile)
    {
        throw 'Step 21 matrix specification truth-boundary contracts are invalid'
    }
    $expectedCells = @(New-PBRemoteVisualStep21ExpectedCells -ComparisonModeClass ([string]$spec.comparisonModeClass) `
        -ComparisonLogicalFps ([UInt32]$spec.comparisonLogicalFps))
    for ($index = 0; $index -lt $expectedCells.Count; $index++)
    {
        Assert-PBMatrixExactKeys -Dictionary $spec.cells[$index] -ExpectedKeys @('cellId', 'profileToken',
            'captureBackend', 'modeClass', 'scaleTarget', 'logicalFps', 'coverageRole', 'profileComparisonRole') `
            -Name "Step 21 matrix cell $index"
        Compare-PBMatrixJsonValue -First $spec.cells[$index] -Second $expectedCells[$index] -Name "Step 21 matrix cell $index"
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $spec }
}

function Import-PBRemoteVisualStep21HardwareScope
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 512KB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 21 hardware scope size or expected SHA-256 is invalid'
    }
    $scope = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 512KB
    Assert-PBMatrixExactKeys -Dictionary $scope -ExpectedKeys @('schema', 'createdUtc', 'scopeId', 'scopeClass',
        'matrixSpecification', 'computerAMonitorCatalog', 'experimentMonitor', 'fullCellCount', 'includedCellCount',
        'includedCells', 'excludedCellCount', 'excludedCells', 'contracts') -Name 'Step 21 hardware scope'
    if ([string]$scope.schema -cne 'PixelBridge.RemoteVisualStep21HardwareScope.1' -or
        [string]$scope.scopeId -cnotmatch '^[0-9a-f]{32}$' -or
        [string]$scope.scopeClass -cne 'Dual2560x1440SingleExperimentMonitor')
    {
        throw 'Step 21 hardware scope schema, identity, or class is invalid'
    }
    foreach ($name in @('matrixSpecification', 'computerAMonitorCatalog'))
    {
        Assert-PBIdentityShape -Identity $scope[$name] -Name "Step 21 hardware scope $name"
        [void](Assert-PBFileIdentity -Path ([string]$scope[$name].path) -Expected $scope[$name] -Name "Step 21 hardware scope $name")
    }
    $matrixSpec = Import-PBRemoteVisualStep21MatrixSpec -Path ([string]$scope.matrixSpecification.path) `
        -ExpectedSha256 ([string]$scope.matrixSpecification.sha256)
    if ([UInt32]$matrixSpec.value.cellCount -ne 41)
    {
        throw 'Step 21 hardware scope requires the canonical 41-cell parent MatrixSpec'
    }
    $monitorCatalog = Read-PBBoundedJson -Path ([string]$scope.computerAMonitorCatalog.path) -MaximumBytes 2MB
    $monitorCount = Get-PBNonNegativeUInt64 -Value $monitorCatalog.monitorCount `
        -Name 'Step 21 hardware scope monitorCount'
    if ([string]$monitorCatalog.schema -cne 'PixelBridge.MonitorCatalog.1' -or $monitorCount -ne 2 -or
        @($monitorCatalog.monitors).Count -ne 2)
    {
        throw 'Step 21 hardware scope requires the sealed two-monitor Computer A catalog'
    }
    $monitorDeviceNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($monitor in @($monitorCatalog.monitors))
    {
        if ([string]::IsNullOrWhiteSpace([string]$monitor.deviceName) -or
            -not $monitorDeviceNames.Add([string]$monitor.deviceName) -or [string]$monitor.rotation -cne 'Identity')
        {
            throw 'Step 21 hardware scope monitor identities must be unique, non-empty, and unrotated'
        }
        $dimensions = Get-PBRectDimensions -Rect $monitor.physicalRect -Name 'Step 21 hardware scope monitor physicalRect'
        $resolutionWidth = Get-PBNonNegativeUInt64 -Value $monitor.resolution.width `
            -Name 'Step 21 hardware scope monitor resolution width'
        $resolutionHeight = Get-PBNonNegativeUInt64 -Value $monitor.resolution.height `
            -Name 'Step 21 hardware scope monitor resolution height'
        if ([UInt32]$dimensions.width -ne 2560 -or [UInt32]$dimensions.height -ne 1440 -or
            $resolutionWidth -ne 2560 -or $resolutionHeight -ne 1440)
        {
            throw 'Step 21 hardware scope is valid only for the authorized two-by-2560x1440 Computer A topology'
        }
    }
    if (Test-PBRectsOverlap -First $monitorCatalog.monitors[0].physicalRect -Second $monitorCatalog.monitors[1].physicalRect)
    {
        throw 'Step 21 hardware scope requires two non-overlapping Computer A monitors'
    }
    Assert-PBMatrixExactKeys -Dictionary $scope.experimentMonitor -ExpectedKeys @('deviceName', 'physicalRect',
        'resolution', 'rotation') -Name 'Step 21 hardware scope experiment monitor'
    $experimentMatches = @($monitorCatalog.monitors | Where-Object { [string]$_.deviceName -ieq [string]$scope.experimentMonitor.deviceName })
    if ($experimentMatches.Count -ne 1)
    {
        throw 'Step 21 hardware scope ExperimentMonitor is not unique in the sealed catalog'
    }
    $experimentMonitor = $experimentMatches[0]
    $expectedExperimentMonitor = [ordered]@{
        deviceName = [string]$experimentMonitor.deviceName
        physicalRect = $experimentMonitor.physicalRect
        resolution = $experimentMonitor.resolution
        rotation = [string]$experimentMonitor.rotation
    }
    Compare-PBMatrixJsonValue -First $scope.experimentMonitor -Second $expectedExperimentMonitor `
        -Name 'Step 21 hardware scope ExperimentMonitor'
    $experimentDimensions = Get-PBRectDimensions -Rect $experimentMonitor.physicalRect `
        -Name 'Step 21 hardware scope ExperimentMonitor physicalRect'
    $partition = New-PBRemoteVisualStep21HardwareScopePartition -Cells @($matrixSpec.value.cells) `
        -ExperimentMonitorWidth ([UInt32]$experimentDimensions.width) -ExperimentMonitorHeight ([UInt32]$experimentDimensions.height)
    $fullCellCount = Get-PBNonNegativeUInt64 -Value $scope.fullCellCount -Name 'Step 21 hardware scope fullCellCount'
    $includedCellCount = Get-PBNonNegativeUInt64 -Value $scope.includedCellCount -Name 'Step 21 hardware scope includedCellCount'
    $excludedCellCount = Get-PBNonNegativeUInt64 -Value $scope.excludedCellCount -Name 'Step 21 hardware scope excludedCellCount'
    if ($fullCellCount -ne 41 -or $includedCellCount -ne 31 -or $excludedCellCount -ne 10 -or
        @($scope.includedCells).Count -ne 31 -or @($scope.excludedCells).Count -ne 10)
    {
        throw 'Step 21 hardware scope must contain the exact authorized 31 included and 10 excluded cells'
    }
    Compare-PBMatrixJsonValue -First @($scope.includedCells) -Second @($partition.includedCells) `
        -Name 'Step 21 hardware scope included cells'
    Compare-PBMatrixJsonValue -First @($scope.excludedCells) -Second @($partition.excludedCells) `
        -Name 'Step 21 hardware scope excluded cells'
    Assert-PBMatrixExactKeys -Dictionary $scope.contracts -ExpectedKeys @('sourceMatrixRemainsCanonical',
        'cellsSelectedOnlyByRoiContainment', 'oneIndependentRunPerIncludedCell', 'excludedCellsDoNotCountAsCoverage',
        'noPostRunScopeMutation', 'certifiedRemoteVisualProfile') -Name 'Step 21 hardware scope contracts'
    if ($scope.contracts.sourceMatrixRemainsCanonical -isnot [bool] -or -not [bool]$scope.contracts.sourceMatrixRemainsCanonical -or
        $scope.contracts.cellsSelectedOnlyByRoiContainment -isnot [bool] -or -not [bool]$scope.contracts.cellsSelectedOnlyByRoiContainment -or
        $scope.contracts.oneIndependentRunPerIncludedCell -isnot [bool] -or -not [bool]$scope.contracts.oneIndependentRunPerIncludedCell -or
        $scope.contracts.excludedCellsDoNotCountAsCoverage -isnot [bool] -or -not [bool]$scope.contracts.excludedCellsDoNotCountAsCoverage -or
        $scope.contracts.noPostRunScopeMutation -isnot [bool] -or -not [bool]$scope.contracts.noPostRunScopeMutation -or
        $scope.contracts.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$scope.contracts.certifiedRemoteVisualProfile)
    {
        throw 'Step 21 hardware scope truth-boundary contracts are invalid'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $scope; matrixSpec = $matrixSpec; monitorCatalog = $monitorCatalog }
}

function New-PBRemoteVisualStep21RunLedgerEntry
{
    param(
        [Parameter(Mandatory = $true)][object]$Cell,
        [Parameter(Mandatory = $true)][UInt32]$Ordinal,
        [Parameter(Mandatory = $true)][object]$ExperimentMonitorRect
    )
    if ($Ordinal -eq 0 -or $Ordinal -gt 256)
    {
        throw 'Step 21 run-ledger ordinal is outside the bounded range'
    }
    $requiredTarget = Get-PBRemoteVisualStep21CellRequiredRoi -Cell $Cell
    $experimentMonitor = Get-PBRectDimensions -Rect $ExperimentMonitorRect `
        -Name 'Step 21 run-ledger ExperimentMonitor physicalRect'
    if ([UInt32]$requiredTarget.width -gt [UInt32]$experimentMonitor.width -or
        [UInt32]$requiredTarget.height -gt [UInt32]$experimentMonitor.height)
    {
        throw 'Step 21 run-ledger cell target does not fit the ExperimentMonitor'
    }
    $captureRoiPurpose = 'LocatorSearchNeighborhood'
    if ([string]$Cell.profileToken -ceq 'remote-lf4')
    {
        $captureRoi = [ordered]@{
            left = [Int64]$experimentMonitor.left
            top = [Int64]$experimentMonitor.top
            right = [Int64]$experimentMonitor.right
            bottom = [Int64]$experimentMonitor.bottom
        }
    }
    elseif ([string]$Cell.profileToken -in @('direct', 'shape'))
    {
        $left = [Int64]$experimentMonitor.left + [Int64][Math]::Floor(([Int64]$experimentMonitor.width - [Int64]$requiredTarget.width) / 2.0)
        $top = [Int64]$experimentMonitor.top + [Int64][Math]::Floor(([Int64]$experimentMonitor.height - [Int64]$requiredTarget.height) / 2.0)
        $captureRoi = [ordered]@{
            left = $left
            top = $top
            right = $left + [Int64]$requiredTarget.width
            bottom = $top + [Int64]$requiredTarget.height
        }
        $captureRoiPurpose = 'ExactProfileCanvas'
    }
    else
    {
        throw 'Step 21 run-ledger cell has an unknown profile token'
    }
    $captureRoiDimensions = Get-PBRectDimensions -Rect $captureRoi -Name 'Step 21 run-ledger Decoder capture ROI'
    if (-not (Test-PBRectContains -Outer $ExperimentMonitorRect -Inner $captureRoi))
    {
        throw 'Step 21 run-ledger Decoder capture ROI escapes the ExperimentMonitor'
    }
    $geometryMode = if ([string]$Cell.scaleTarget -ceq '1.000') { 'Strict1To1' } else { 'LocatorScaled' }
    return [ordered]@{
        ordinal = $Ordinal
        cellId = [string]$Cell.cellId
        profileToken = [string]$Cell.profileToken
        captureBackend = [string]$Cell.captureBackend
        modeClass = [string]$Cell.modeClass
        scaleTarget = [string]$Cell.scaleTarget
        logicalFps = [UInt32]$Cell.logicalFps
        coverageRole = [string]$Cell.coverageRole
        profileComparisonRole = [string]$Cell.profileComparisonRole
        geometryMode = $geometryMode
        minimumTargetCanvas = [ordered]@{ width = [UInt32]$requiredTarget.width; height = [UInt32]$requiredTarget.height }
        decoderCaptureRoi = $captureRoi
        decoderCaptureRoiArgument = "$($captureRoi.left),$($captureRoi.top),$($captureRoiDimensions.width),$($captureRoiDimensions.height)"
        captureRoiPurpose = $captureRoiPurpose
        runDirectoryName = ('cell-{0:D3}-{1}' -f $Ordinal, [string]$Cell.cellId)
        runId = $null
        executionStatus = 'PENDING'
        artifactsMustBeIndependent = $true
    }
}

function Import-PBRemoteVisualStep21RunLedger
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 2MB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 21 run ledger size or expected SHA-256 is invalid'
    }
    $ledger = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 2MB
    Assert-PBMatrixExactKeys -Dictionary $ledger -ExpectedKeys @('schema', 'createdUtc', 'status', 'ledgerId',
        'matrixSpecification', 'hardwareScope', 'computerAMonitorCatalog', 'experimentMonitor', 'fullCellCount',
        'includedCellCount', 'entries', 'excludedCellCount', 'excludedCells', 'contracts', 'truthBoundary') `
        -Name 'Step 21 run ledger'
    if ([string]$ledger.schema -cne 'PixelBridge.RemoteVisualStep21RunLedger.1' -or
        [string]$ledger.status -cne 'NOT_EXECUTED' -or [string]$ledger.ledgerId -cnotmatch '^[0-9a-f]{32}$')
    {
        throw 'Step 21 run ledger schema, status, or identity is invalid'
    }
    foreach ($name in @('matrixSpecification', 'hardwareScope', 'computerAMonitorCatalog'))
    {
        Assert-PBIdentityShape -Identity $ledger[$name] -Name "Step 21 run ledger $name"
        [void](Assert-PBFileIdentity -Path ([string]$ledger[$name].path) -Expected $ledger[$name] `
            -Name "Step 21 run ledger $name")
    }
    $matrixSpec = Import-PBRemoteVisualStep21MatrixSpec -Path ([string]$ledger.matrixSpecification.path) `
        -ExpectedSha256 ([string]$ledger.matrixSpecification.sha256)
    $hardwareScope = Import-PBRemoteVisualStep21HardwareScope -Path ([string]$ledger.hardwareScope.path) `
        -ExpectedSha256 ([string]$ledger.hardwareScope.sha256)
    Assert-PBMatrixIdentityEqual -Actual $hardwareScope.value.matrixSpecification -Expected $ledger.matrixSpecification `
        -Name 'Step 21 run ledger parent MatrixSpec'
    Assert-PBMatrixIdentityEqual -Actual $hardwareScope.value.computerAMonitorCatalog -Expected $ledger.computerAMonitorCatalog `
        -Name 'Step 21 run ledger Computer A monitor catalog'
    Compare-PBMatrixJsonValue -First $ledger.experimentMonitor -Second $hardwareScope.value.experimentMonitor `
        -Name 'Step 21 run ledger ExperimentMonitor'
    $fullCellCount = Get-PBNonNegativeUInt64 -Value $ledger.fullCellCount -Name 'Step 21 run ledger fullCellCount'
    $includedCellCount = Get-PBNonNegativeUInt64 -Value $ledger.includedCellCount -Name 'Step 21 run ledger includedCellCount'
    $excludedCellCount = Get-PBNonNegativeUInt64 -Value $ledger.excludedCellCount -Name 'Step 21 run ledger excludedCellCount'
    if ($fullCellCount -ne 41 -or $includedCellCount -ne 31 -or $excludedCellCount -ne 10 -or
        @($ledger.entries).Count -ne 31 -or @($ledger.excludedCells).Count -ne 10)
    {
        throw 'Step 21 run ledger must preserve the exact 31 included and 10 excluded hardware scope'
    }
    for ($index = 0; $index -lt 31; $index++)
    {
        $expectedEntry = New-PBRemoteVisualStep21RunLedgerEntry -Cell $hardwareScope.value.includedCells[$index] `
            -Ordinal ([UInt32]($index + 1)) -ExperimentMonitorRect $hardwareScope.value.experimentMonitor.physicalRect
        Compare-PBMatrixJsonValue -First $ledger.entries[$index] -Second $expectedEntry `
            -Name "Step 21 run-ledger entry $index"
    }
    Compare-PBMatrixJsonValue -First @($ledger.excludedCells) -Second @($hardwareScope.value.excludedCells) `
        -Name 'Step 21 run-ledger excluded cells'
    Assert-PBMatrixExactKeys -Dictionary $ledger.contracts -ExpectedKeys @('sourceMatrixAndScopeImmutable',
        'oneIndependentRunPerIncludedCell', 'runIdAllocatedOnlyWhenCellExecutionBegins',
        'perRunDeploymentUiPlanAndEvidenceRequired', 'captureRoiDoesNotEstablishScale',
        'failedRunsMustBeClassifiedAndRetained') -Name 'Step 21 run-ledger contracts'
    if ($ledger.contracts.sourceMatrixAndScopeImmutable -isnot [bool] -or -not [bool]$ledger.contracts.sourceMatrixAndScopeImmutable -or
        $ledger.contracts.oneIndependentRunPerIncludedCell -isnot [bool] -or -not [bool]$ledger.contracts.oneIndependentRunPerIncludedCell -or
        $ledger.contracts.runIdAllocatedOnlyWhenCellExecutionBegins -isnot [bool] -or -not [bool]$ledger.contracts.runIdAllocatedOnlyWhenCellExecutionBegins -or
        $ledger.contracts.perRunDeploymentUiPlanAndEvidenceRequired -isnot [bool] -or -not [bool]$ledger.contracts.perRunDeploymentUiPlanAndEvidenceRequired -or
        $ledger.contracts.captureRoiDoesNotEstablishScale -isnot [bool] -or -not [bool]$ledger.contracts.captureRoiDoesNotEstablishScale -or
        $ledger.contracts.failedRunsMustBeClassifiedAndRetained -isnot [bool] -or -not [bool]$ledger.contracts.failedRunsMustBeClassifiedAndRetained)
    {
        throw 'Step 21 run-ledger execution contracts are invalid'
    }
    Assert-PBMatrixExactKeys -Dictionary $ledger.truthBoundary -ExpectedKeys @('formalStep21Accepted',
        'executedCellCount', 'pendingCellCount', 'full41CoverageCompleted', 'excludedCellsDoNotCountAsCoverage',
        'acceptedLocatorGeometryAuthority', 'computerBMonitorCatalogStillRequired',
        'providerUiEvidenceStillRequiredPerRun', 'certifiedRemoteVisualProfile', 'statement') `
        -Name 'Step 21 run-ledger truth boundary'
    $executedCellCount = Get-PBNonNegativeUInt64 -Value $ledger.truthBoundary.executedCellCount `
        -Name 'Step 21 run-ledger executedCellCount'
    $pendingCellCount = Get-PBNonNegativeUInt64 -Value $ledger.truthBoundary.pendingCellCount `
        -Name 'Step 21 run-ledger pendingCellCount'
    $expectedStatement = 'This ledger freezes an operator schedule only; no Step 21 matrix cell, provider behavior, file recovery, or certification is claimed.'
    if ($ledger.truthBoundary.formalStep21Accepted -isnot [bool] -or [bool]$ledger.truthBoundary.formalStep21Accepted -or
        $executedCellCount -ne 0 -or $pendingCellCount -ne 31 -or
        $ledger.truthBoundary.full41CoverageCompleted -isnot [bool] -or [bool]$ledger.truthBoundary.full41CoverageCompleted -or
        $ledger.truthBoundary.excludedCellsDoNotCountAsCoverage -isnot [bool] -or -not [bool]$ledger.truthBoundary.excludedCellsDoNotCountAsCoverage -or
        [string]$ledger.truthBoundary.acceptedLocatorGeometryAuthority -cne 'AcceptedBootstrapLocatorPixels' -or
        $ledger.truthBoundary.computerBMonitorCatalogStillRequired -isnot [bool] -or -not [bool]$ledger.truthBoundary.computerBMonitorCatalogStillRequired -or
        $ledger.truthBoundary.providerUiEvidenceStillRequiredPerRun -isnot [bool] -or -not [bool]$ledger.truthBoundary.providerUiEvidenceStillRequiredPerRun -or
        $ledger.truthBoundary.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$ledger.truthBoundary.certifiedRemoteVisualProfile -or
        [string]$ledger.truthBoundary.statement -cne $expectedStatement)
    {
        throw 'Step 21 run-ledger truth boundary is invalid'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $ledger; matrixSpec = $matrixSpec; hardwareScope = $hardwareScope }
}

function Import-PBRemoteVisualStep21RunLedgerSeal
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 512KB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 21 run-ledger seal size or expected SHA-256 is invalid'
    }
    $seal = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 512KB
    Assert-PBMatrixExactKeys -Dictionary $seal -ExpectedKeys @('schema', 'createdUtc', 'status', 'ledgerId',
        'matrixId', 'scopeId', 'includedCellCount', 'excludedCellCount', 'artifactCount', 'artifacts',
        'formalStep21Accepted', 'certifiedRemoteVisualProfile') -Name 'Step 21 run-ledger seal'
    $includedCellCount = Get-PBNonNegativeUInt64 -Value $seal.includedCellCount `
        -Name 'Step 21 run-ledger seal includedCellCount'
    $excludedCellCount = Get-PBNonNegativeUInt64 -Value $seal.excludedCellCount `
        -Name 'Step 21 run-ledger seal excludedCellCount'
    $artifactCount = Get-PBNonNegativeUInt64 -Value $seal.artifactCount -Name 'Step 21 run-ledger seal artifactCount'
    if ([string]$seal.schema -cne 'PixelBridge.RemoteVisualStep21RunLedgerSeal.1' -or
        [string]$seal.status -cne 'NOT_EXECUTED' -or [string]$seal.ledgerId -cnotmatch '^[0-9a-f]{32}$' -or
        [string]$seal.matrixId -cnotmatch '^[0-9a-f]{32}$' -or [string]$seal.scopeId -cnotmatch '^[0-9a-f]{32}$' -or
        $includedCellCount -ne 31 -or $excludedCellCount -ne 10 -or $artifactCount -ne 5 -or
        @($seal.artifacts).Count -ne 5 -or $seal.formalStep21Accepted -isnot [bool] -or
        [bool]$seal.formalStep21Accepted -or $seal.certifiedRemoteVisualProfile -isnot [bool] -or
        [bool]$seal.certifiedRemoteVisualProfile)
    {
        throw 'Step 21 run-ledger seal schema, counts, or truth boundary is invalid'
    }
    $artifactMaximumBytes = @([UInt64](256KB), [UInt64](512KB), [UInt64](2MB), [UInt64](2MB), [UInt64](1MB))
    for ($index = 0; $index -lt @($seal.artifacts).Count; $index++)
    {
        $artifact = $seal.artifacts[$index]
        Assert-PBMatrixBoundedIdentity -Identity $artifact -MaximumBytes $artifactMaximumBytes[$index] `
            -Name "Step 21 run-ledger sealed artifact $index"
        [void](Assert-PBFileIdentity -Path ([string]$artifact.path) -Expected $artifact `
            -Name 'Step 21 run-ledger sealed artifact')
    }
    $ledger = Import-PBRemoteVisualStep21RunLedger -Path ([string]$seal.artifacts[3].path) `
        -ExpectedSha256 ([string]$seal.artifacts[3].sha256)
    Assert-PBMatrixIdentityEqual -Actual $seal.artifacts[0] -Expected $ledger.matrixSpec.identity `
        -Name 'Step 21 run-ledger seal MatrixSpec'
    Assert-PBMatrixIdentityEqual -Actual $seal.artifacts[1] -Expected $ledger.hardwareScope.identity `
        -Name 'Step 21 run-ledger seal HardwareScope'
    Assert-PBMatrixIdentityEqual -Actual $seal.artifacts[2] -Expected $ledger.value.computerAMonitorCatalog `
        -Name 'Step 21 run-ledger seal Computer A monitor catalog'
    Assert-PBMatrixIdentityEqual -Actual $seal.artifacts[3] -Expected $ledger.identity `
        -Name 'Step 21 run-ledger seal ledger'
    $csvPath = [System.IO.Path]::GetFullPath([string]$seal.artifacts[4].path)
    if ([System.IO.Path]::GetFileName($csvPath) -cne 'step21-run-ledger.csv' -or
        [System.IO.Path]::GetDirectoryName($csvPath) -cne [System.IO.Path]::GetDirectoryName($ledger.path))
    {
        throw 'Step 21 run-ledger seal CSV path is not beside the ledger'
    }
    $csvRows = @(Import-Csv -LiteralPath $csvPath)
    $expectedColumns = @('ordinal', 'cellId', 'profileToken', 'captureBackend', 'modeClass', 'scaleTarget',
        'logicalFps', 'coverageRole', 'profileComparisonRole', 'geometryMode', 'minimumTargetWidth',
        'minimumTargetHeight', 'decoderCaptureRoiLeft', 'decoderCaptureRoiTop', 'decoderCaptureRoiRight',
        'decoderCaptureRoiBottom', 'decoderCaptureRoiArgument', 'captureRoiPurpose', 'runDirectoryName',
        'executionStatus', 'runId')
    if ($csvRows.Count -ne 31 -or @($csvRows[0].PSObject.Properties.Name).Count -ne $expectedColumns.Count)
    {
        throw 'Step 21 run-ledger seal CSV row or column count is invalid'
    }
    for ($columnIndex = 0; $columnIndex -lt $expectedColumns.Count; $columnIndex++)
    {
        if ([string]$csvRows[0].PSObject.Properties.Name[$columnIndex] -cne $expectedColumns[$columnIndex])
        {
            throw 'Step 21 run-ledger seal CSV columns are invalid'
        }
    }
    for ($index = 0; $index -lt $csvRows.Count; $index++)
    {
        $entry = $ledger.value.entries[$index]
        $row = $csvRows[$index]
        if ([string]$row.ordinal -cne [string]$entry.ordinal -or [string]$row.cellId -cne [string]$entry.cellId -or
            [string]$row.profileToken -cne [string]$entry.profileToken -or
            [string]$row.captureBackend -cne [string]$entry.captureBackend -or
            [string]$row.modeClass -cne [string]$entry.modeClass -or [string]$row.scaleTarget -cne [string]$entry.scaleTarget -or
            [string]$row.logicalFps -cne [string]$entry.logicalFps -or
            [string]$row.coverageRole -cne [string]$entry.coverageRole -or
            [string]$row.profileComparisonRole -cne [string]$entry.profileComparisonRole -or
            [string]$row.geometryMode -cne [string]$entry.geometryMode -or
            [string]$row.minimumTargetWidth -cne [string]$entry.minimumTargetCanvas.width -or
            [string]$row.minimumTargetHeight -cne [string]$entry.minimumTargetCanvas.height -or
            [string]$row.decoderCaptureRoiLeft -cne [string]$entry.decoderCaptureRoi.left -or
            [string]$row.decoderCaptureRoiTop -cne [string]$entry.decoderCaptureRoi.top -or
            [string]$row.decoderCaptureRoiRight -cne [string]$entry.decoderCaptureRoi.right -or
            [string]$row.decoderCaptureRoiBottom -cne [string]$entry.decoderCaptureRoi.bottom -or
            [string]$row.decoderCaptureRoiArgument -cne [string]$entry.decoderCaptureRoiArgument -or
            [string]$row.captureRoiPurpose -cne [string]$entry.captureRoiPurpose -or
            [string]$row.runDirectoryName -cne [string]$entry.runDirectoryName -or
            [string]$row.executionStatus -cne 'PENDING' -or -not [string]::IsNullOrEmpty([string]$row.runId))
        {
            throw "Step 21 run-ledger seal CSV row $index differs from the ledger"
        }
    }
    if ([string]$seal.ledgerId -cne [string]$ledger.value.ledgerId -or
        [string]$seal.matrixId -cne [string]$ledger.matrixSpec.value.matrixId -or
        [string]$seal.scopeId -cne [string]$ledger.hardwareScope.value.scopeId)
    {
        throw 'Step 21 run-ledger seal identity tuple differs from the ledger'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $seal; ledger = $ledger; csvIdentity = $seal.artifacts[4] }
}

function Import-PBRemoteVisualStep21ComputerBMonitorCatalog
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 2MB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Computer B Step 21 monitor catalog size or expected SHA-256 is invalid'
    }
    $catalog = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 2MB
    Assert-PBMatrixExactKeys -Dictionary $catalog -ExpectedKeys @('schema', 'monitorCount', 'monitors') `
        -Name 'Computer B Step 21 monitor catalog'
    $monitorCount = Get-PBNonNegativeUInt64 -Value $catalog.monitorCount `
        -Name 'Computer B Step 21 monitor catalog monitorCount'
    if ([string]$catalog.schema -cne 'PixelBridge.MonitorCatalog.1' -or $monitorCount -ne 2 -or
        @($catalog.monitors).Count -ne 2)
    {
        throw 'Computer B Step 21 requires exactly two monitor catalog entries'
    }
    $deviceNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($monitor in @($catalog.monitors))
    {
        if ($monitor -isnot [System.Collections.IDictionary] -or -not $monitor.Contains('deviceName') -or
            -not $monitor.Contains('physicalRect') -or -not $monitor.Contains('resolution') -or
            -not $monitor.Contains('rotation') -or [string]::IsNullOrWhiteSpace([string]$monitor.deviceName) -or
            -not $deviceNames.Add([string]$monitor.deviceName) -or [string]$monitor.rotation -cne 'Identity')
        {
            throw 'Computer B Step 21 monitor identities must be unique, non-empty, and unrotated'
        }
        $dimensions = Get-PBRectDimensions -Rect $monitor.physicalRect -Name 'Computer B Step 21 monitor physicalRect'
        foreach ($coordinate in @($dimensions.left, $dimensions.top, $dimensions.right, $dimensions.bottom))
        {
            if ([Int64]$coordinate -lt [Int32]::MinValue -or [Int64]$coordinate -gt [Int32]::MaxValue)
            {
                throw 'Computer B Step 21 monitor coordinates exceed the Windows signed 32-bit desktop domain'
            }
        }
        if ($monitor.resolution -isnot [System.Collections.IDictionary] -or
            -not $monitor.resolution.Contains('width') -or -not $monitor.resolution.Contains('height'))
        {
            throw 'Computer B Step 21 monitor resolution is missing'
        }
        $resolutionWidth = Get-PBNonNegativeUInt64 -Value $monitor.resolution.width `
            -Name 'Computer B Step 21 monitor resolution width'
        $resolutionHeight = Get-PBNonNegativeUInt64 -Value $monitor.resolution.height `
            -Name 'Computer B Step 21 monitor resolution height'
        if ([UInt32]$dimensions.width -ne 1920 -or [UInt32]$dimensions.height -ne 1080 -or
            $resolutionWidth -ne 1920 -or $resolutionHeight -ne 1080)
        {
            throw 'Computer B Step 21 requires two exact 1920x1080 monitors'
        }
    }
    if (Test-PBRectsOverlap -First $catalog.monitors[0].physicalRect -Second $catalog.monitors[1].physicalRect)
    {
        throw 'Computer B Step 21 requires two non-overlapping monitors in Extend mode'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $catalog }
}

function New-PBRemoteVisualStep21NormalizedMonitor
{
    param(
        [Parameter(Mandatory = $true)][object]$Monitor,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $rect = Get-PBRectDimensions -Rect $Monitor.physicalRect -Name "$Name physicalRect"
    $resolutionWidth = Get-PBNonNegativeUInt64 -Value $Monitor.resolution.width -Name "$Name resolution width"
    $resolutionHeight = Get-PBNonNegativeUInt64 -Value $Monitor.resolution.height -Name "$Name resolution height"
    return [ordered]@{
        deviceName = [string]$Monitor.deviceName
        physicalRect = [ordered]@{ left = $rect.left; top = $rect.top; right = $rect.right; bottom = $rect.bottom }
        resolution = [ordered]@{ width = $resolutionWidth; height = $resolutionHeight }
        rotation = [string]$Monitor.rotation
    }
}

function New-PBRemoteVisualStep21EndpointTopologyValue
{
    param(
        [Parameter(Mandatory = $true)][object]$RunLedger,
        [Parameter(Mandatory = $true)][object]$ComputerBCatalog,
        [Parameter(Mandatory = $true)][string]$ComputerBProtectedMonitorDeviceName,
        [Parameter(Mandatory = $true)][string]$ComputerBExperimentMonitorDeviceName
    )
    $computerACatalog = $RunLedger.hardwareScope.monitorCatalog
    $computerAExperimentName = [string]$RunLedger.value.experimentMonitor.deviceName
    $computerAExperimentMatches = @($computerACatalog.monitors | Where-Object {
        [string]$_.deviceName -ieq $computerAExperimentName
    })
    $computerAProtectedMatches = @($computerACatalog.monitors | Where-Object {
        [string]$_.deviceName -ine $computerAExperimentName
    })
    if ($computerAExperimentMatches.Count -ne 1 -or $computerAProtectedMatches.Count -ne 1)
    {
        throw 'Step 21 endpoint scope cannot resolve the unique Computer A monitor pair'
    }
    if ([string]::IsNullOrWhiteSpace($ComputerBProtectedMonitorDeviceName) -or
        [string]::IsNullOrWhiteSpace($ComputerBExperimentMonitorDeviceName) -or
        $ComputerBProtectedMonitorDeviceName -ieq $ComputerBExperimentMonitorDeviceName)
    {
        throw 'Step 21 endpoint scope requires distinct Computer B Protected and Experiment monitors'
    }
    $computerBProtectedMatches = @($ComputerBCatalog.monitors | Where-Object {
        [string]$_.deviceName -ieq $ComputerBProtectedMonitorDeviceName
    })
    $computerBExperimentMatches = @($ComputerBCatalog.monitors | Where-Object {
        [string]$_.deviceName -ieq $ComputerBExperimentMonitorDeviceName
    })
    if ($computerBProtectedMatches.Count -ne 1 -or $computerBExperimentMatches.Count -ne 1)
    {
        throw 'Step 21 endpoint scope cannot resolve the selected Computer B monitor pair'
    }
    $lf4Entries = @($RunLedger.value.entries | Where-Object { [string]$_.profileToken -ceq 'remote-lf4' })
    $baselineEntries = @($RunLedger.value.entries | Where-Object { [string]$_.profileToken -in @('direct', 'shape') })
    if ($lf4Entries.Count -ne 29 -or $baselineEntries.Count -ne 2)
    {
        throw 'Step 21 endpoint scope requires the canonical 29 LF4 and two baseline ledger entries'
    }
    foreach ($entry in $lf4Entries)
    {
        if ([string]$entry.decoderCaptureRoiArgument -cne [string]$lf4Entries[0].decoderCaptureRoiArgument -or
            [string]$entry.captureRoiPurpose -cne 'LocatorSearchNeighborhood')
        {
            throw 'Step 21 endpoint scope LF4 ledger capture policy is inconsistent'
        }
    }
    foreach ($entry in $baselineEntries)
    {
        if ([string]$entry.decoderCaptureRoiArgument -cne [string]$baselineEntries[0].decoderCaptureRoiArgument -or
            [string]$entry.captureRoiPurpose -cne 'ExactProfileCanvas')
        {
            throw 'Step 21 endpoint scope baseline ledger capture policy is inconsistent'
        }
    }
    $computerAProtected = New-PBRemoteVisualStep21NormalizedMonitor -Monitor $computerAProtectedMatches[0] `
        -Name 'Computer A ProtectedMonitor'
    $computerAExperiment = New-PBRemoteVisualStep21NormalizedMonitor -Monitor $computerAExperimentMatches[0] `
        -Name 'Computer A ExperimentMonitor'
    $computerBProtected = New-PBRemoteVisualStep21NormalizedMonitor -Monitor $computerBProtectedMatches[0] `
        -Name 'Computer B ProtectedMonitor'
    $computerBExperiment = New-PBRemoteVisualStep21NormalizedMonitor -Monitor $computerBExperimentMatches[0] `
        -Name 'Computer B ExperimentMonitor'
    return [ordered]@{
        computerA = [ordered]@{
            protectedMonitor = $computerAProtected
            experimentMonitor = $computerAExperiment
            decoderCapturePolicies = [ordered]@{
                lf4 = [ordered]@{
                    purpose = 'LocatorSearchNeighborhood'
                    physicalRect = $lf4Entries[0].decoderCaptureRoi
                    argument = [string]$lf4Entries[0].decoderCaptureRoiArgument
                }
                directShape = [ordered]@{
                    purpose = 'ExactProfileCanvas'
                    physicalRect = $baselineEntries[0].decoderCaptureRoi
                    argument = [string]$baselineEntries[0].decoderCaptureRoiArgument
                }
            }
        }
        computerB = [ordered]@{
            protectedMonitor = $computerBProtected
            experimentMonitor = $computerBExperiment
            encoderDataWindow = $computerBExperiment.physicalRect
            encoderOrigin = [ordered]@{ x = [Int64]$computerBExperiment.physicalRect.left; y = [Int64]$computerBExperiment.physicalRect.top }
        }
    }
}

function Import-PBRemoteVisualStep21EndpointScope
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 2MB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 21 endpoint scope size or expected SHA-256 is invalid'
    }
    $scope = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 2MB
    Assert-PBMatrixExactKeys -Dictionary $scope -ExpectedKeys @('schema', 'createdUtc', 'status', 'endpointScopeId',
        'runLedger', 'runLedgerSeal', 'matrixSpecification', 'hardwareScope', 'computerAMonitorCatalog',
        'computerBMonitorCatalog', 'topology', 'contracts', 'truthBoundary') -Name 'Step 21 endpoint scope'
    if ([string]$scope.schema -cne 'PixelBridge.RemoteVisualStep21EndpointScope.1' -or
        [string]$scope.status -cne 'READINESS_ONLY' -or [string]$scope.endpointScopeId -cnotmatch '^[0-9a-f]{32}$')
    {
        throw 'Step 21 endpoint scope schema, status, or identity is invalid'
    }
    $identityMaximumBytes = [ordered]@{
        runLedger = [UInt64](2MB)
        runLedgerSeal = [UInt64](512KB)
        matrixSpecification = [UInt64](256KB)
        hardwareScope = [UInt64](512KB)
        computerAMonitorCatalog = [UInt64](2MB)
        computerBMonitorCatalog = [UInt64](2MB)
    }
    foreach ($name in $identityMaximumBytes.Keys)
    {
        Assert-PBMatrixBoundedIdentity -Identity $scope[$name] -MaximumBytes $identityMaximumBytes[$name] `
            -Name "Step 21 endpoint scope $name"
        [void](Assert-PBFileIdentity -Path ([string]$scope[$name].path) -Expected $scope[$name] `
            -Name "Step 21 endpoint scope $name")
    }
    $ledgerSeal = Import-PBRemoteVisualStep21RunLedgerSeal -Path ([string]$scope.runLedgerSeal.path) `
        -ExpectedSha256 ([string]$scope.runLedgerSeal.sha256)
    $ledger = $ledgerSeal.ledger
    $computerB = Import-PBRemoteVisualStep21ComputerBMonitorCatalog -Path ([string]$scope.computerBMonitorCatalog.path) `
        -ExpectedSha256 ([string]$scope.computerBMonitorCatalog.sha256)
    Assert-PBMatrixIdentityEqual -Actual $scope.runLedger -Expected $ledger.identity -Name 'Step 21 endpoint scope RunLedger'
    Assert-PBMatrixIdentityEqual -Actual $scope.matrixSpecification -Expected $ledger.matrixSpec.identity `
        -Name 'Step 21 endpoint scope MatrixSpec'
    Assert-PBMatrixIdentityEqual -Actual $scope.hardwareScope -Expected $ledger.hardwareScope.identity `
        -Name 'Step 21 endpoint scope HardwareScope'
    Assert-PBMatrixIdentityEqual -Actual $scope.computerAMonitorCatalog -Expected $ledger.value.computerAMonitorCatalog `
        -Name 'Step 21 endpoint scope Computer A monitor catalog'
    $expectedTopology = New-PBRemoteVisualStep21EndpointTopologyValue -RunLedger $ledger `
        -ComputerBCatalog $computerB.value `
        -ComputerBProtectedMonitorDeviceName ([string]$scope.topology.computerB.protectedMonitor.deviceName) `
        -ComputerBExperimentMonitorDeviceName ([string]$scope.topology.computerB.experimentMonitor.deviceName)
    Compare-PBMatrixJsonValue -First $scope.topology -Second $expectedTopology -Name 'Step 21 endpoint-scope topology'
    Assert-PBMatrixExactKeys -Dictionary $scope.contracts -ExpectedKeys @('runLedgerAndParentsImmutable',
        'twoNonOverlappingExtendedMonitorsPerEndpoint', 'dataPixelsConfinedToExperimentMonitors',
        'freshLiveMonitorCatalogRequiredPerRun', 'perRunDeploymentAndUiEvidenceRequired',
        'noFileClipboardOrIpcPayloadSideChannel') -Name 'Step 21 endpoint-scope contracts'
    if ($scope.contracts.runLedgerAndParentsImmutable -isnot [bool] -or -not [bool]$scope.contracts.runLedgerAndParentsImmutable -or
        $scope.contracts.twoNonOverlappingExtendedMonitorsPerEndpoint -isnot [bool] -or -not [bool]$scope.contracts.twoNonOverlappingExtendedMonitorsPerEndpoint -or
        $scope.contracts.dataPixelsConfinedToExperimentMonitors -isnot [bool] -or -not [bool]$scope.contracts.dataPixelsConfinedToExperimentMonitors -or
        $scope.contracts.freshLiveMonitorCatalogRequiredPerRun -isnot [bool] -or -not [bool]$scope.contracts.freshLiveMonitorCatalogRequiredPerRun -or
        $scope.contracts.perRunDeploymentAndUiEvidenceRequired -isnot [bool] -or -not [bool]$scope.contracts.perRunDeploymentAndUiEvidenceRequired -or
        $scope.contracts.noFileClipboardOrIpcPayloadSideChannel -isnot [bool] -or -not [bool]$scope.contracts.noFileClipboardOrIpcPayloadSideChannel)
    {
        throw 'Step 21 endpoint-scope contracts are invalid'
    }
    Assert-PBMatrixExactKeys -Dictionary $scope.truthBoundary -ExpectedKeys @('topologyReadinessOnly',
        'formalStep21Accepted', 'executedCellCount', 'runIdsAllocated', 'providerUiEvidenceStillRequiredPerRun',
        'liveCatalogRevalidationStillRequiredPerRun', 'certifiedRemoteVisualProfile', 'statement') `
        -Name 'Step 21 endpoint-scope truth boundary'
    $executedCellCount = Get-PBNonNegativeUInt64 -Value $scope.truthBoundary.executedCellCount `
        -Name 'Step 21 endpoint-scope executedCellCount'
    $runIdsAllocated = Get-PBNonNegativeUInt64 -Value $scope.truthBoundary.runIdsAllocated `
        -Name 'Step 21 endpoint-scope runIdsAllocated'
    $expectedStatement = 'This scope binds readiness catalogs and monitor roles only; it is not a live endpoint preflight, a matrix cell, file recovery evidence, or certification.'
    if ($scope.truthBoundary.topologyReadinessOnly -isnot [bool] -or -not [bool]$scope.truthBoundary.topologyReadinessOnly -or
        $scope.truthBoundary.formalStep21Accepted -isnot [bool] -or [bool]$scope.truthBoundary.formalStep21Accepted -or
        $executedCellCount -ne 0 -or $runIdsAllocated -ne 0 -or
        $scope.truthBoundary.providerUiEvidenceStillRequiredPerRun -isnot [bool] -or -not [bool]$scope.truthBoundary.providerUiEvidenceStillRequiredPerRun -or
        $scope.truthBoundary.liveCatalogRevalidationStillRequiredPerRun -isnot [bool] -or -not [bool]$scope.truthBoundary.liveCatalogRevalidationStillRequiredPerRun -or
        $scope.truthBoundary.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$scope.truthBoundary.certifiedRemoteVisualProfile -or
        [string]$scope.truthBoundary.statement -cne $expectedStatement)
    {
        throw 'Step 21 endpoint-scope truth boundary is invalid'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $scope; ledgerSeal = $ledgerSeal; computerB = $computerB }
}

function Import-PBRemoteVisualStep21EndpointScopeSeal
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if ([UInt64]$identity.size -gt 512KB -or
        (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256))
    {
        throw 'Step 21 endpoint-scope seal size or expected SHA-256 is invalid'
    }
    $seal = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 512KB
    Assert-PBMatrixExactKeys -Dictionary $seal -ExpectedKeys @('schema', 'createdUtc', 'status', 'endpointScopeId',
        'ledgerId', 'matrixId', 'scopeId', 'artifactCount', 'artifacts', 'executedCellCount',
        'formalStep21Accepted', 'certifiedRemoteVisualProfile') -Name 'Step 21 endpoint-scope seal'
    $artifactCount = Get-PBNonNegativeUInt64 -Value $seal.artifactCount `
        -Name 'Step 21 endpoint-scope seal artifactCount'
    $executedCellCount = Get-PBNonNegativeUInt64 -Value $seal.executedCellCount `
        -Name 'Step 21 endpoint-scope seal executedCellCount'
    if ([string]$seal.schema -cne 'PixelBridge.RemoteVisualStep21EndpointScopeSeal.1' -or
        [string]$seal.status -cne 'READINESS_ONLY' -or [string]$seal.endpointScopeId -cnotmatch '^[0-9a-f]{32}$' -or
        [string]$seal.ledgerId -cnotmatch '^[0-9a-f]{32}$' -or [string]$seal.matrixId -cnotmatch '^[0-9a-f]{32}$' -or
        [string]$seal.scopeId -cnotmatch '^[0-9a-f]{32}$' -or $artifactCount -ne 7 -or
        @($seal.artifacts).Count -ne 7 -or $executedCellCount -ne 0 -or
        $seal.formalStep21Accepted -isnot [bool] -or [bool]$seal.formalStep21Accepted -or
        $seal.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$seal.certifiedRemoteVisualProfile)
    {
        throw 'Step 21 endpoint-scope seal schema, counts, or truth boundary is invalid'
    }
    $artifactMaximumBytes = @([UInt64](2MB), [UInt64](512KB), [UInt64](256KB), [UInt64](512KB),
        [UInt64](2MB), [UInt64](2MB), [UInt64](2MB))
    for ($index = 0; $index -lt @($seal.artifacts).Count; $index++)
    {
        $artifact = $seal.artifacts[$index]
        Assert-PBMatrixBoundedIdentity -Identity $artifact -MaximumBytes $artifactMaximumBytes[$index] `
            -Name "Step 21 endpoint-scope sealed artifact $index"
        [void](Assert-PBFileIdentity -Path ([string]$artifact.path) -Expected $artifact `
            -Name 'Step 21 endpoint-scope sealed artifact')
    }
    $scope = Import-PBRemoteVisualStep21EndpointScope -Path ([string]$seal.artifacts[6].path) `
        -ExpectedSha256 ([string]$seal.artifacts[6].sha256)
    $expectedIdentities = @(
        $scope.value.runLedger,
        $scope.value.runLedgerSeal,
        $scope.value.matrixSpecification,
        $scope.value.hardwareScope,
        $scope.value.computerAMonitorCatalog,
        $scope.value.computerBMonitorCatalog,
        $scope.identity)
    for ($index = 0; $index -lt $expectedIdentities.Count; $index++)
    {
        Assert-PBMatrixIdentityEqual -Actual $seal.artifacts[$index] -Expected $expectedIdentities[$index] `
            -Name "Step 21 endpoint-scope seal artifact $index"
    }
    if ([string]$seal.endpointScopeId -cne [string]$scope.value.endpointScopeId -or
        [string]$seal.ledgerId -cne [string]$scope.ledgerSeal.ledger.value.ledgerId -or
        [string]$seal.matrixId -cne [string]$scope.ledgerSeal.ledger.matrixSpec.value.matrixId -or
        [string]$seal.scopeId -cne [string]$scope.ledgerSeal.ledger.hardwareScope.value.scopeId)
    {
        throw 'Step 21 endpoint-scope seal identity tuple differs from the scope'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $seal; scope = $scope }
}

function Get-PBNullableFiniteNumber
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$NonNegative
    )
    if ($null -eq $Value)
    {
        return $null
    }
    if ($Value -is [bool] -or $Value -isnot [ValueType])
    {
        throw "$Name must be null or numeric"
    }
    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or ($NonNegative -and $number -lt 0.0))
    {
        throw "$Name must be finite$(if ($NonNegative) { ' and non-negative' } else { '' })"
    }
    return $number
}

function Get-PBNonNegativeUInt64
{
    param(
        [Parameter(Mandatory = $false)][AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Value -or $Value -is [bool] -or $Value -isnot [ValueType])
    {
        throw "$Name must be a non-negative integer"
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
        if ($number -lt 0 -or $number -gt [decimal][UInt64]::MaxValue -or [decimal]::Truncate($number) -ne $number)
        {
            throw 'out of range or fractional'
        }
        return [UInt64]$number
    }
    catch
    {
        throw "$Name must be a non-negative integer"
    }
}

function Get-PBRemoteVisualObservedLocatorGeometry
{
    param([Parameter(Mandatory = $true)][object]$DecoderReport)
    if ($DecoderReport -isnot [System.Collections.IDictionary] -or
        -not $DecoderReport.Contains('observedLocatorGeometry') -or
        -not $DecoderReport.Contains('telemetryBootstrapSuccesses') -or
        $DecoderReport.observedLocatorGeometry -isnot [System.Collections.IDictionary])
    {
        throw 'Decoder report lacks authoritative observed Locator geometry'
    }
    $geometry = $DecoderReport.observedLocatorGeometry
    $metricNames = @('lastOriginX', 'lastOriginY', 'lastScaleX', 'lastScaleY', 'lastMarkerResidualPixels',
        'minimumOriginX', 'maximumOriginX', 'minimumOriginY', 'maximumOriginY', 'minimumScaleX', 'maximumScaleX',
        'minimumScaleY', 'maximumScaleY', 'minimumMarkerResidualPixels', 'maximumMarkerResidualPixels',
        'maximumScaleAnisotropy')
    Assert-PBMatrixExactKeys -Dictionary $geometry -ExpectedKeys (@('authority', 'samples') + $metricNames) `
        -Name 'Decoder observed Locator geometry'
    if ([string]$geometry.authority -cne 'AcceptedBootstrapLocatorPixels')
    {
        throw 'Decoder observed Locator geometry has a non-authoritative source'
    }
    $samples = Get-PBNonNegativeUInt64 -Value $geometry.samples -Name 'observed Locator geometry samples'
    $bootstrapSuccesses = Get-PBNonNegativeUInt64 -Value $DecoderReport.telemetryBootstrapSuccesses `
        -Name 'telemetryBootstrapSuccesses'
    if ($samples -ne $bootstrapSuccesses)
    {
        throw 'Observed Locator geometry samples differ from successful Bootstrap telemetry'
    }
    $normalized = [ordered]@{ authority = 'AcceptedBootstrapLocatorPixels'; samples = $samples }
    foreach ($name in $metricNames)
    {
        $nonNegative = $name -like '*Scale*' -or $name -like '*Residual*'
        $number = Get-PBNullableFiniteNumber -Value $geometry[$name] -Name "observed Locator geometry $name" `
            -NonNegative:$nonNegative
        if ($name -like '*Scale*' -and $null -ne $number -and $name -cne 'maximumScaleAnisotropy' -and
            ($number -le 0.0 -or $number -gt 16.0))
        {
            throw "Observed Locator geometry $name is outside (0,16]"
        }
        if (($name -like '*Residual*' -or $name -ceq 'maximumScaleAnisotropy') -and
            $null -ne $number -and $number -gt 16.0)
        {
            throw "Observed Locator geometry $name exceeds 16 pixels/scale units"
        }
        if (($samples -ne 0) -ne ($null -ne $number))
        {
            throw "Observed Locator geometry $name has invalid null semantics for its sample count"
        }
        $normalized[$name] = $number
    }
    if ($samples -eq 0)
    {
        return $normalized
    }
    foreach ($axis in @('OriginX', 'OriginY', 'ScaleX', 'ScaleY', 'MarkerResidualPixels'))
    {
        $minimum = [double]$normalized["minimum$axis"]
        $maximum = [double]$normalized["maximum$axis"]
        $last = [double]$normalized["last$axis"]
        if ($minimum -gt $maximum)
        {
            throw "Observed Locator geometry $axis range is inverted"
        }
        if ($last -lt $minimum -or $last -gt $maximum)
        {
            throw "Observed Locator geometry last$axis is outside its observed range"
        }
    }
    $lastAnisotropy = [Math]::Abs([double]$normalized.lastScaleX - [double]$normalized.lastScaleY)
    if ([double]$normalized.maximumScaleAnisotropy + 0.000000000000001 -lt $lastAnisotropy)
    {
        throw 'Observed Locator geometry maximumScaleAnisotropy is below the last sample'
    }
    return $normalized
}

function Assert-PBRemoteVisualObservedLocatorTarget
{
    param(
        [Parameter(Mandatory = $true)][object]$Geometry,
        [Parameter(Mandatory = $true)][string]$ScaleTarget,
        [Parameter(Mandatory = $true)][string]$ProfileToken,
        [string]$Context = 'Successful matrix run'
    )
    if ([UInt64]$Geometry.samples -eq 0)
    {
        throw "$Context lacks accepted Locator geometry"
    }
    if ([double]$Geometry.maximumScaleAnisotropy -gt 0.015)
    {
        throw "$Context exceeds the frozen 0.015 Locator scale anisotropy tolerance"
    }
    foreach ($pair in @(
        @([double]$Geometry.lastScaleX, [double]$Geometry.lastScaleY),
        @([double]$Geometry.minimumScaleX, [double]$Geometry.minimumScaleY),
        @([double]$Geometry.maximumScaleX, [double]$Geometry.maximumScaleY)))
    {
        if ((Get-PBStep21ScaleTarget -ScaleX $pair[0] -ScaleY $pair[1] -ProfileToken $ProfileToken) -cne $ScaleTarget)
        {
            throw "$Context Locator geometry differs from the frozen scale target"
        }
    }
}

function Get-PBRemoteVisualAuthoritativeMetrics
{
    param([Parameter(Mandatory = $true)][object]$CombinedReport)
    if ([string]$CombinedReport.schema -cne 'PixelBridge.RemoteVisualCombinedReport.1' -or
        $CombinedReport.successfulRun -isnot [bool] -or $CombinedReport.evidenceValid -isnot [bool] -or
        $CombinedReport.encoder -isnot [System.Collections.IDictionary] -or
        $CombinedReport.decoder -isnot [System.Collections.IDictionary] -or
        $CombinedReport.externalVerification -isnot [System.Collections.IDictionary])
    {
        throw 'Combined report cannot provide authoritative Step 21 metrics'
    }
    $encoder = $CombinedReport.encoder
    $decoder = $CombinedReport.decoder
    $remoteMetrics = $decoder.remoteMetricTelemetry
    $outer = $decoder.outerAdmission
    $captureStall = $decoder.captureStall
    $visualStall = $decoder.visualStall
    if ($remoteMetrics -isnot [System.Collections.IDictionary] -or $outer -isnot [System.Collections.IDictionary] -or
        $captureStall -isnot [System.Collections.IDictionary] -or $visualStall -isnot [System.Collections.IDictionary] -or
        $decoder.wholeFileDigestVerified -isnot [bool] -or $decoder.finalPublishSucceeded -isnot [bool] -or
        ($null -ne $CombinedReport.externalVerification.match -and $CombinedReport.externalVerification.match -isnot [bool]))
    {
        throw 'Combined report is missing metric, Outer admission, or stall denominators'
    }
    $falseAccepted = $decoder.falseAcceptedCodewords
    if ($null -ne $falseAccepted -and (Get-PBNonNegativeUInt64 -Value $falseAccepted -Name 'falseAcceptedCodewords') -ne 0)
    {
        throw 'Step 21 evidence reports nonzero false accepted codewords'
    }
    $outerConflictRejections = Get-PBNonNegativeUInt64 -Value $outer.conflictRejections -Name 'outerConflictRejections'
    if ($outerConflictRejections -ne 0)
    {
        throw 'Step 21 evidence reports an Outer symbol conflict'
    }
    $observedLocatorGeometry = Get-PBRemoteVisualObservedLocatorGeometry -DecoderReport $decoder
    return [ordered]@{
        successfulRun = [bool]$CombinedReport.successfulRun
        evidenceValid = [bool]$CombinedReport.evidenceValid
        encoderState = [string]$encoder.state
        decoderState = [string]$decoder.state
        configuredLogicalVisualFps = [UInt32](Get-PBNonNegativeUInt64 -Value $encoder.configuredLogicalVisualFps -Name 'configuredLogicalVisualFps')
        generatedVisualFramesPerSecond = Get-PBNullableFiniteNumber -Value $encoder.generatedVisualFramesPerSecond -Name 'generatedVisualFramesPerSecond' -NonNegative
        generatedPayloadBytesPerSecond = Get-PBNullableFiniteNumber -Value $encoder.generatedPayloadBytesPerSecond -Name 'generatedPayloadBytesPerSecond' -NonNegative
        captureFps = Get-PBNullableFiniteNumber -Value $decoder.captureFps -Name 'captureFps' -NonNegative
        uniqueVisualFps = Get-PBNullableFiniteNumber -Value $decoder.uniqueVisualFps -Name 'uniqueVisualFps' -NonNegative
        endToEndUniqueVisualFps = Get-PBNullableFiniteNumber -Value $decoder.endToEndUniqueVisualFps -Name 'endToEndUniqueVisualFps' -NonNegative
        bootstrapSuccessRate = Get-PBNullableFiniteNumber -Value $decoder.bootstrapSuccessRate -Name 'bootstrapSuccessRate' -NonNegative
        observedLocatorGeometry = $observedLocatorGeometry
        preFecBerEstimate = Get-PBNullableFiniteNumber -Value $decoder.preFecBerEstimate -Name 'preFecBerEstimate' -NonNegative
        fecFrameErrorRate = Get-PBNullableFiniteNumber -Value $decoder.fecFrameErrorRate -Name 'fecFrameErrorRate' -NonNegative
        fecCodewordFailureRate = Get-PBNullableFiniteNumber -Value $decoder.fecCodewordFailureRate -Name 'fecCodewordFailureRate' -NonNegative
        evaluatedDataFrames = Get-PBNonNegativeUInt64 -Value $decoder.evaluatedDataFrames -Name 'evaluatedDataFrames'
        evaluatedCodewords = Get-PBNonNegativeUInt64 -Value $decoder.evaluatedCodewords -Name 'evaluatedCodewords'
        fecAcceptedTransportBlocks = Get-PBNonNegativeUInt64 -Value $decoder.fecAcceptedTransportBlocks -Name 'fecAcceptedTransportBlocks'
        temporallyAdmittedTransportBlocks = Get-PBNonNegativeUInt64 -Value $decoder.temporallyAdmittedTransportBlocks -Name 'temporallyAdmittedTransportBlocks'
        remoteMetricFrames = Get-PBNonNegativeUInt64 -Value $remoteMetrics.frames -Name 'remoteMetricFrames'
        remoteMetricSamples = Get-PBNonNegativeUInt64 -Value $remoteMetrics.samples -Name 'remoteMetricSamples'
        remoteMetricZeroMagnitudeRate = Get-PBNullableFiniteNumber -Value $remoteMetrics.zeroMagnitudeRate -Name 'remoteMetricZeroMagnitudeRate' -NonNegative
        remoteMetricMeanAbsoluteMetric = Get-PBNullableFiniteNumber -Value $remoteMetrics.meanAbsoluteMetric -Name 'remoteMetricMeanAbsoluteMetric' -NonNegative
        remoteSymbolSamples = Get-PBNonNegativeUInt64 -Value $remoteMetrics.symbolSamples -Name 'remoteSymbolSamples'
        remoteUnreliableSymbols = Get-PBNonNegativeUInt64 -Value $remoteMetrics.unreliableSymbols -Name 'remoteUnreliableSymbols'
        remoteUnreliableSymbolRate = Get-PBNullableFiniteNumber -Value $remoteMetrics.unreliableSymbolRate -Name 'remoteUnreliableSymbolRate' -NonNegative
        remoteRejectedMetricFrames = Get-PBNonNegativeUInt64 -Value $remoteMetrics.rejectedFrames -Name 'remoteRejectedMetricFrames'
        remoteStaleRegions = Get-PBNonNegativeUInt64 -Value $remoteMetrics.staleRegions -Name 'remoteStaleRegions'
        remoteFreshnessTagMismatches = Get-PBNonNegativeUInt64 -Value $remoteMetrics.freshnessTagMismatches -Name 'remoteFreshnessTagMismatches'
        remoteFreshnessTagErasures = Get-PBNonNegativeUInt64 -Value $remoteMetrics.freshnessTagErasures -Name 'remoteFreshnessTagErasures'
        duplicateFrameSequences = Get-PBNonNegativeUInt64 -Value $decoder.duplicateFrameSequences -Name 'duplicateFrameSequences'
        reorderedFrameSequences = Get-PBNonNegativeUInt64 -Value $decoder.reorderedFrameSequences -Name 'reorderedFrameSequences'
        frameSequenceGapEvents = Get-PBNonNegativeUInt64 -Value $decoder.frameSequenceGapEvents -Name 'frameSequenceGapEvents'
        skippedFrameSequences = Get-PBNonNegativeUInt64 -Value $decoder.skippedFrameSequences -Name 'skippedFrameSequences'
        captureStallCount = Get-PBNonNegativeUInt64 -Value $captureStall.count -Name 'captureStallCount'
        visualStallCount = Get-PBNonNegativeUInt64 -Value $visualStall.count -Name 'visualStallCount'
        verifiedEncodedGoodputBitsPerSecond = Get-PBNullableFiniteNumber -Value $decoder.verifiedEncodedGoodputBitsPerSecond -Name 'verifiedEncodedGoodputBitsPerSecond' -NonNegative
        wholeFileDigestVerified = [bool]$decoder.wholeFileDigestVerified
        finalPublishSucceeded = [bool]$decoder.finalPublishSucceeded
        externalMatch = if ($null -eq $CombinedReport.externalVerification.match) { $null } else { [bool]$CombinedReport.externalVerification.match }
        falseAcceptedCodewords = $falseAccepted
        outerConflictRejections = $outerConflictRejections
    }
}

function Compare-PBMatrixJsonValue
{
    param(
        [Parameter(Mandatory = $true)][object]$First,
        [Parameter(Mandatory = $true)][object]$Second,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $firstJson = $First | ConvertTo-Json -Depth 20 -Compress
    $secondJson = $Second | ConvertTo-Json -Depth 20 -Compress
    if ($firstJson -cne $secondJson)
    {
        throw "$Name differs from the authoritative combined report"
    }
}

function New-PBRemoteVisualMatrixRunRecordValue
{
    param(
        [Parameter(Mandatory = $true)][object]$FrozenPlan,
        [Parameter(Mandatory = $true)][ValidateSet('Success', 'Failure')][string]$Outcome,
        [Parameter(Mandatory = $true)][ValidateSet('success', 'geometry', 'signal', 'temporal', 'metric', 'scheduler')][string]$FailureClassification,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$ReplayPath,
        [Parameter(Mandatory = $true)][string]$CombinedReportPath,
        [Parameter(Mandatory = $true)][string]$EncoderReportPath,
        [Parameter(Mandatory = $true)][string]$LiveDecoderReportPath,
        [Parameter(Mandatory = $true)][string]$OfflineDecoderReportPath,
        [Parameter(Mandatory = $true)][string]$OutcomeVerificationPath,
        [AllowNull()][string]$InspectionPath
    )
    $plan = $FrozenPlan.value
    if ([string]$plan.schema -cne 'PixelBridge.RemoteVisualPilotPlan.3')
    {
        throw 'Current matrix run records require a Step 21 PilotPlan.3'
    }
    if (($Outcome -ceq 'Success') -ne ($FailureClassification -ceq 'success'))
    {
        throw 'Matrix outcome and failure classification disagree'
    }
    if (($Outcome -ceq 'Failure') -ne (-not [string]::IsNullOrWhiteSpace($InspectionPath)))
    {
        throw 'Only classified-failure matrix records require a Replay inspection artifact'
    }
    $combinedPath = [System.IO.Path]::GetFullPath($CombinedReportPath)
    $combined = Read-PBBoundedJson -Path $combinedPath -MaximumBytes 8MB
    if ([string]$combined.runId -cne [string]$plan.runId -or [string]$combined.profile -cne [string]$plan.profileName -or
        [bool]$combined.successfulRun -ne ($Outcome -ceq 'Success'))
    {
        throw 'Combined report identity or outcome disagrees with the Step 21 plan'
    }
    $metrics = Get-PBRemoteVisualAuthoritativeMetrics -CombinedReport $combined
    if (-not [bool]$metrics.evidenceValid)
    {
        throw 'Matrix run requires complete endpoint evidence journals'
    }
    $scaleTarget = [string]$plan.matrix.scaleTarget
    if ($scaleTarget -notin @('0.750', '1.000', '1.259', '1.500') -or
        [string]$plan.remoteGeometry.expectedLocatorScaleTarget -cne $scaleTarget)
    {
        throw 'Matrix plan lacks one explicit Locator scale target'
    }
    $observedGeometry = $metrics.observedLocatorGeometry
    if ($Outcome -ceq 'Success')
    {
        Assert-PBRemoteVisualObservedLocatorTarget -Geometry $observedGeometry -ScaleTarget $scaleTarget `
            -ProfileToken ([string]$plan.profileToken) -Context 'Successful matrix run'
    }
    $actualSource = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($SourcePath))
    if ([UInt64]$actualSource.size -ne [UInt64]$plan.source.size -or [string]$actualSource.sha256 -cne [string]$plan.source.sha256)
    {
        throw 'Matrix source identity differs from the frozen plan'
    }
    $inspectionIdentity = $null
    if (-not [string]::IsNullOrWhiteSpace($InspectionPath))
    {
        $inspectionIdentity = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($InspectionPath))
    }
    return [ordered]@{
        schema = 'PixelBridge.RemoteVisualMatrixRunRecord.2'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        runId = [string]$plan.runId
        outcome = $Outcome
        failureClassification = $FailureClassification
        profileToken = [string]$plan.profileToken
        profileName = [string]$plan.profileName
        visualProfileId = [UInt64]$plan.visualProfileId
        visualLayoutVersion = [UInt32]$plan.visualLayoutVersion
        matrix = [ordered]@{
            modeClass = [string]$plan.matrix.modeClass
            visibleRemoteMode = [string]$plan.matrix.visibleRemoteMode
            captureBackend = [string]$plan.policy.captureBackend
            backendCoverageRole = [string]$plan.matrix.backendCoverageRole
            profileComparisonRole = [string]$plan.matrix.profileComparisonRole
            logicalFps = [UInt32]$plan.logicalFps
            geometryMode = [string]$plan.geometryMode
            scaleTarget = $scaleTarget
            observedLocatorGeometry = $observedGeometry
            runIsolation = [string]$plan.matrix.runIsolation
        }
        contracts = [ordered]@{
            noSilentResample = $true
            captureRoiIsNotScaleEvidence = $true
            geometryAuthority = 'AcceptedBootstrapLocatorPixels'
            unknownChromaAndLatencyRemainUnknown = $true
            differentRunMetricsMustNotBeMerged = $true
            authoritativeMetricSource = 'PixelBridge.RemoteVisualCombinedReport.1 Decoder/Receiver fields'
            certifiedRemoteVisualProfile = $false
        }
        identities = [ordered]@{
            plan = $FrozenPlan.identity
            deployment = Get-PBFileIdentity -Path ([string]$plan.deployment.manifest.path)
            packageManifest = Get-PBFileIdentity -Path ([string]$plan.deployment.packageManifest.path)
            source = $actualSource
            remoteUiEvidence = Get-PBFileIdentity -Path ([string]$plan.remoteUi.evidence.path)
            encoderEnvironment = Get-PBFileIdentity -Path ([string]$plan.deployment.encoderEnvironment.path)
            decoderEnvironment = Get-PBFileIdentity -Path ([string]$plan.deployment.decoderEnvironment.path)
            replay = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($ReplayPath))
            combinedReport = Get-PBFileIdentity -Path $combinedPath
            outcomeVerification = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($OutcomeVerificationPath))
            inspection = $inspectionIdentity
            encoderReport = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($EncoderReportPath))
            liveDecoderReport = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($LiveDecoderReportPath))
            offlineDecoderReport = Get-PBFileIdentity -Path ([System.IO.Path]::GetFullPath($OfflineDecoderReportPath))
        }
        authoritativeMetrics = $metrics
    }
}

function Import-PBRemoteVisualMatrixRunRecord
{
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ExpectedSha256 = ''
    )
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $identity = Get-PBFileIdentity -Path $resolvedPath
    if (-not [string]::IsNullOrEmpty($ExpectedSha256) -and [string]$identity.sha256 -cne $ExpectedSha256)
    {
        throw 'Matrix run record SHA-256 mismatch'
    }
    $record = Read-PBBoundedJson -Path $resolvedPath -MaximumBytes 4MB
    Assert-PBMatrixExactKeys -Dictionary $record -ExpectedKeys @('schema', 'createdUtc', 'runId', 'outcome',
        'failureClassification', 'profileToken', 'profileName', 'visualProfileId', 'visualLayoutVersion', 'matrix',
        'contracts', 'identities', 'authoritativeMetrics') -Name 'matrix run record'
    if ([string]$record.schema -cne 'PixelBridge.RemoteVisualMatrixRunRecord.2' -or
        [string]$record.runId -cnotmatch '^[0-9a-f]{32}$' -or [string]$record.outcome -notin @('Success', 'Failure') -or
        [string]$record.failureClassification -notin @('success', 'geometry', 'signal', 'temporal', 'metric', 'scheduler') -or
        (([string]$record.outcome -ceq 'Success') -ne ([string]$record.failureClassification -ceq 'success')))
    {
        throw 'Matrix run record schema, RunId, outcome, or classification is invalid'
    }
    Assert-PBMatrixExactKeys -Dictionary $record.matrix -ExpectedKeys @('modeClass', 'visibleRemoteMode', 'captureBackend',
        'backendCoverageRole', 'profileComparisonRole', 'logicalFps', 'geometryMode', 'scaleTarget',
        'observedLocatorGeometry', 'runIsolation') -Name 'matrix run tuple'
    Assert-PBMatrixExactKeys -Dictionary $record.contracts -ExpectedKeys @('noSilentResample',
        'captureRoiIsNotScaleEvidence', 'geometryAuthority', 'unknownChromaAndLatencyRemainUnknown',
        'differentRunMetricsMustNotBeMerged', 'authoritativeMetricSource', 'certifiedRemoteVisualProfile') `
        -Name 'matrix run contracts'
    Assert-PBMatrixExactKeys -Dictionary $record.identities -ExpectedKeys @('plan', 'deployment', 'packageManifest', 'source',
        'remoteUiEvidence', 'encoderEnvironment', 'decoderEnvironment', 'replay', 'combinedReport', 'outcomeVerification',
        'inspection', 'encoderReport', 'liveDecoderReport', 'offlineDecoderReport') -Name 'matrix run identities'
    if ($record.contracts.noSilentResample -isnot [bool] -or -not [bool]$record.contracts.noSilentResample -or
        $record.contracts.captureRoiIsNotScaleEvidence -isnot [bool] -or
        -not [bool]$record.contracts.captureRoiIsNotScaleEvidence -or
        [string]$record.contracts.geometryAuthority -cne 'AcceptedBootstrapLocatorPixels' -or
        $record.contracts.unknownChromaAndLatencyRemainUnknown -isnot [bool] -or -not [bool]$record.contracts.unknownChromaAndLatencyRemainUnknown -or
        $record.contracts.differentRunMetricsMustNotBeMerged -isnot [bool] -or -not [bool]$record.contracts.differentRunMetricsMustNotBeMerged -or
        [string]$record.contracts.authoritativeMetricSource -cne 'PixelBridge.RemoteVisualCombinedReport.1 Decoder/Receiver fields' -or
        $record.contracts.certifiedRemoteVisualProfile -isnot [bool] -or [bool]$record.contracts.certifiedRemoteVisualProfile)
    {
        throw 'Matrix run truth-boundary contracts are invalid'
    }
    foreach ($name in @('plan', 'deployment', 'packageManifest', 'source', 'remoteUiEvidence', 'encoderEnvironment',
        'decoderEnvironment', 'replay', 'combinedReport', 'outcomeVerification', 'encoderReport', 'liveDecoderReport',
        'offlineDecoderReport'))
    {
        [void](Assert-PBFileIdentity -Path ([string]$record.identities[$name].path) -Expected $record.identities[$name] -Name "matrix $name")
    }
    if ($null -ne $record.identities.inspection)
    {
        [void](Assert-PBFileIdentity -Path ([string]$record.identities.inspection.path) -Expected $record.identities.inspection -Name 'matrix inspection')
    }
    $plan = Import-PBRemoteVisualPilotPlan -Path ([string]$record.identities.plan.path) -ExpectedSha256 ([string]$record.identities.plan.sha256)
    if ([string]$plan.value.schema -cne 'PixelBridge.RemoteVisualPilotPlan.3' -or
        [string]$record.runId -cne [string]$plan.value.runId -or [string]$record.profileToken -cne [string]$plan.value.profileToken -or
        [string]$record.profileName -cne [string]$plan.value.profileName -or [UInt64]$record.visualProfileId -ne [UInt64]$plan.value.visualProfileId -or
        [UInt32]$record.visualLayoutVersion -ne [UInt32]$plan.value.visualLayoutVersion)
    {
        throw 'Matrix run record profile identity differs from its frozen plan'
    }
    if ([string]$record.matrix.modeClass -cne [string]$plan.value.matrix.modeClass -or
        [string]$record.matrix.visibleRemoteMode -cne [string]$plan.value.matrix.visibleRemoteMode -or
        [string]$record.matrix.captureBackend -cne [string]$plan.value.policy.captureBackend -or
        [string]$record.matrix.backendCoverageRole -cne [string]$plan.value.matrix.backendCoverageRole -or
        [string]$record.matrix.profileComparisonRole -cne [string]$plan.value.matrix.profileComparisonRole -or
        [UInt32]$record.matrix.logicalFps -ne [UInt32]$plan.value.logicalFps -or
        [string]$record.matrix.geometryMode -cne [string]$plan.value.geometryMode -or
        [string]$record.matrix.runIsolation -cne [string]$plan.value.matrix.runIsolation -or
        [string]$record.matrix.scaleTarget -cne [string]$plan.value.matrix.scaleTarget -or
        [string]$record.matrix.scaleTarget -cne [string]$plan.value.remoteGeometry.expectedLocatorScaleTarget)
    {
        throw 'Matrix run tuple differs from its frozen plan or scale grid'
    }
    if (([string]$record.outcome -ceq 'Failure') -ne ($null -ne $record.identities.inspection))
    {
        throw 'Matrix outcome and Replay inspection identity disagree'
    }
    Assert-PBMatrixIdentityEqual -Actual $record.identities.deployment -Expected $plan.value.deployment.manifest -Name 'matrix deployment'
    Assert-PBMatrixIdentityEqual -Actual $record.identities.packageManifest -Expected $plan.value.deployment.packageManifest -Name 'matrix package manifest'
    Assert-PBMatrixIdentityEqual -Actual $record.identities.remoteUiEvidence -Expected $plan.value.remoteUi.evidence -Name 'matrix UI evidence'
    Assert-PBMatrixIdentityEqual -Actual $record.identities.encoderEnvironment -Expected $plan.value.deployment.encoderEnvironment -Name 'matrix Encoder environment'
    Assert-PBMatrixIdentityEqual -Actual $record.identities.decoderEnvironment -Expected $plan.value.deployment.decoderEnvironment -Name 'matrix Decoder environment'
    if ([UInt64]$record.identities.source.size -ne [UInt64]$plan.value.source.size -or
        [string]$record.identities.source.sha256 -cne [string]$plan.value.source.sha256)
    {
        throw 'Matrix source identity differs from its frozen plan'
    }
    $combined = Read-PBBoundedJson -Path ([string]$record.identities.combinedReport.path) -MaximumBytes 8MB
    if ([string]$combined.runId -cne [string]$record.runId -or [string]$combined.profile -cne [string]$record.profileName -or
        [bool]$combined.successfulRun -ne ([string]$record.outcome -ceq 'Success'))
    {
        throw 'Matrix combined report outcome or identity mismatch'
    }
    $expectedMetrics = Get-PBRemoteVisualAuthoritativeMetrics -CombinedReport $combined
    Compare-PBMatrixJsonValue -First $record.authoritativeMetrics -Second $expectedMetrics -Name 'matrix authoritative metrics'
    Compare-PBMatrixJsonValue -First $record.matrix.observedLocatorGeometry `
        -Second $expectedMetrics.observedLocatorGeometry -Name 'matrix observed Locator geometry'
    if ([string]$record.outcome -ceq 'Success')
    {
        $observedGeometry = $expectedMetrics.observedLocatorGeometry
        Assert-PBRemoteVisualObservedLocatorTarget -Geometry $observedGeometry `
            -ScaleTarget ([string]$record.matrix.scaleTarget) -ProfileToken ([string]$record.profileToken) `
            -Context 'Successful matrix record'
    }
    $verification = Read-PBBoundedJson -Path ([string]$record.identities.outcomeVerification.path) -MaximumBytes 4MB
    if ([string]$record.outcome -ceq 'Success')
    {
        if ([string]$verification.schema -cne 'PixelBridge.RemoteVisualPilotEvidenceVerification.1' -or
            [string]$verification.status -cne 'PASS' -or [string]$verification.runId -cne [string]$record.runId -or
            [string]$verification.completion.failureClassification -cne 'success' -or
            [string]$verification.profileToken -cne [string]$record.profileToken -or
            [string]$verification.captureBackend -cne [string]$record.matrix.captureBackend)
        {
            throw 'Matrix success verification is invalid'
        }
        Compare-PBMatrixJsonValue -First $verification.matrix -Second $plan.value.matrix -Name 'matrix success verification tuple'
        Assert-PBMatrixIdentityEqual -Actual $verification.plan -Expected $record.identities.plan -Name 'matrix success plan'
        Assert-PBMatrixIdentityEqual -Actual $verification.replayIdentity -Expected $record.identities.replay -Name 'matrix success Replay'
        Assert-PBMatrixIdentityEqual -Actual $verification.combinedReport -Expected $record.identities.combinedReport -Name 'matrix success combined report'
        Assert-PBMatrixIdentityEqual -Actual $verification.endpointReports.encoder -Expected $record.identities.encoderReport -Name 'matrix success Encoder report'
        Assert-PBMatrixIdentityEqual -Actual $verification.endpointReports.liveDecoder -Expected $record.identities.liveDecoderReport -Name 'matrix success live Decoder report'
        Assert-PBMatrixIdentityEqual -Actual $verification.endpointReports.offlineDecoder -Expected $record.identities.offlineDecoderReport -Name 'matrix success offline Decoder report'
    }
    elseif ([string]$verification.schema -cne 'PixelBridge.RemoteVisualFieldFailureVerification.1' -or
        [string]$verification.status -cne 'PASS' -or [string]$verification.runId -cne [string]$record.runId -or
        [string]$verification.failureClassification -cne [string]$record.failureClassification -or
        [string]$verification.profileToken -cne [string]$record.profileToken -or
        [string]$verification.captureBackend -cne [string]$record.matrix.captureBackend)
    {
        throw 'Matrix failure verification is invalid'
    }
    else
    {
        Compare-PBMatrixJsonValue -First $verification.matrix -Second $plan.value.matrix -Name 'matrix failure verification tuple'
        Assert-PBMatrixIdentityEqual -Actual $verification.plan -Expected $record.identities.plan -Name 'matrix failure plan'
        Assert-PBMatrixIdentityEqual -Actual $verification.replay -Expected $record.identities.replay -Name 'matrix failure Replay'
        Assert-PBMatrixIdentityEqual -Actual $verification.replayInspection -Expected $record.identities.inspection -Name 'matrix failure Replay inspection'
        Assert-PBMatrixIdentityEqual -Actual $verification.combinedReport -Expected $record.identities.combinedReport -Name 'matrix failure combined report'
        Assert-PBMatrixIdentityEqual -Actual $verification.endpointReports.encoder -Expected $record.identities.encoderReport -Name 'matrix failure Encoder report'
        Assert-PBMatrixIdentityEqual -Actual $verification.endpointReports.liveDecoder -Expected $record.identities.liveDecoderReport -Name 'matrix failure live Decoder report'
        Assert-PBMatrixIdentityEqual -Actual $verification.endpointReports.offlineDecoder -Expected $record.identities.offlineDecoderReport -Name 'matrix failure offline Decoder report'
    }
    return [ordered]@{ path = $resolvedPath; identity = $identity; value = $record; plan = $plan; combined = $combined }
}

function Add-PBFailureSupport
{
    param(
        [Parameter(Mandatory = $true)][hashtable]$Support,
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Evidence,
        [UInt64]$Count = 1
    )
    $Support[$Category].count = [UInt64]$Support[$Category].count + $Count
    [void]$Support[$Category].evidence.Add($Evidence)
}

function Get-PBRemoteVisualFailureSupport
{
    param(
        [Parameter(Mandatory = $true)][object]$Inspection,
        [Parameter(Mandatory = $true)][object]$EncoderReport,
        [Parameter(Mandatory = $true)][object]$LiveDecoderReport,
        [Parameter(Mandatory = $true)][object]$OfflineDecoderReport
    )
    if ([string]$Inspection.schema -cne 'PixelBridge.RemoteVisualReplayInspection.1' -or
        $Inspection.frames -isnot [System.Collections.IEnumerable] -or
        [string]$LiveDecoderReport.role -cne 'Decoder' -or [string]$OfflineDecoderReport.role -cne 'Decoder')
    {
        throw 'Failure support requires one valid Replay inspection and two Decoder reports'
    }
    $support = @{}
    foreach ($category in @('geometry', 'signal', 'temporal', 'metric', 'scheduler'))
    {
        $support[$category] = [ordered]@{ count = [UInt64]0; evidence = [Collections.Generic.List[string]]::new() }
    }
    $geometryErasures = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in @('ScaleOutOfRange', 'AlignmentOutOfRange', 'FrameOutOfBounds', 'InvalidGeometry', 'AmbiguousGeometry'))
    {
        [void]$geometryErasures.Add($name)
    }
    $signalErasures = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in @('MarkersNotFound', 'IncompleteMarkers', 'LowContrast', 'BootstrapFecFailure', 'BootstrapCrcFailure',
        'BootstrapMismatch', 'TimingMismatch', 'DoubleImage', 'ExcessResidual', 'BootstrapErasure', 'PilotClipping',
        'PilotOrder', 'PilotVariance', 'PilotSpatialMismatch', 'PixelReadFailure'))
    {
        [void]$signalErasures.Add($name)
    }
    foreach ($frame in @($Inspection.frames))
    {
        $bootstrapErasure = [string]$frame.bootstrap.erasure
        $modulationErasure = [string]$frame.modulation.erasure
        if ($geometryErasures.Contains($bootstrapErasure) -or $geometryErasures.Contains($modulationErasure))
        {
            Add-PBFailureSupport -Support $support -Category geometry -Evidence "Replay frame $($frame.ordinal): $bootstrapErasure/$modulationErasure"
        }
        if ($signalErasures.Contains($bootstrapErasure) -or $signalErasures.Contains($modulationErasure))
        {
            Add-PBFailureSupport -Support $support -Category signal -Evidence "Replay frame $($frame.ordinal): $bootstrapErasure/$modulationErasure"
        }
        $fecFailures = [UInt64]$frame.transport.fecFailures
        $crcFailures = [UInt64]$frame.transport.crcFailures
        $identityFailures = [UInt64]$frame.transport.identityFailures
        if ($fecFailures + $crcFailures + $identityFailures -gt 0)
        {
            Add-PBFailureSupport -Support $support -Category signal -Evidence "Replay frame $($frame.ordinal): FEC/CRC/identity failures" `
                -Count ($fecFailures + $crcFailures + $identityFailures)
            Add-PBFailureSupport -Support $support -Category metric -Evidence "Replay frame $($frame.ordinal): transport rejection under decoded soft metrics" `
                -Count ($fecFailures + $crcFailures + $identityFailures)
        }
        $unreliable = [UInt64]$frame.modulation.unreliablePrimary
        if ($null -ne $frame.modulation.unreliableSecondary)
        {
            $unreliable += [UInt64]$frame.modulation.unreliableSecondary
        }
        if ($unreliable -gt 0)
        {
            Add-PBFailureSupport -Support $support -Category metric -Evidence "Replay frame $($frame.ordinal): unreliable modulation work units" -Count $unreliable
        }
        $temporalCount = [UInt64]0
        foreach ($name in @('staleRegions', 'freshnessTagMismatches', 'freshnessTagErasures', 'erasedDataMetrics'))
        {
            if ($null -ne $frame.modulation[$name])
            {
                $temporalCount += [UInt64]$frame.modulation[$name]
            }
        }
        if ($temporalCount -gt 0)
        {
            Add-PBFailureSupport -Support $support -Category temporal -Evidence "Replay frame $($frame.ordinal): stale/freshness evidence" -Count $temporalCount
        }
    }
    $remoteMetrics = $LiveDecoderReport.remoteMetricTelemetry
    $reportTemporal = [UInt64]$LiveDecoderReport.duplicateFrameSequences + [UInt64]$LiveDecoderReport.reorderedFrameSequences +
        [UInt64]$LiveDecoderReport.frameSequenceGapEvents + [UInt64]$LiveDecoderReport.skippedFrameSequences +
        [UInt64]$remoteMetrics.staleRegions + [UInt64]$remoteMetrics.freshnessTagMismatches + [UInt64]$remoteMetrics.freshnessTagErasures
    if ($reportTemporal -gt 0)
    {
        Add-PBFailureSupport -Support $support -Category temporal -Evidence 'Live Decoder temporal admission/stale counters' -Count $reportTemporal
    }
    $reportMetric = [UInt64]$remoteMetrics.unreliableSymbols + [UInt64]$remoteMetrics.rejectedFrames + [UInt64]$LiveDecoderReport.fecFailures +
        [UInt64]$LiveDecoderReport.crcFailures + [UInt64]$LiveDecoderReport.identityFailures
    if ($reportMetric -gt 0)
    {
        Add-PBFailureSupport -Support $support -Category metric -Evidence 'Live Decoder unreliable/rejected/FEC metric counters' -Count $reportMetric
    }
    $schedulerCount = [UInt64]$LiveDecoderReport.captureDroppedFrames + [UInt64]$LiveDecoderReport.captureAcquireTimeouts +
        [UInt64]$LiveDecoderReport.captureReadbackDropEvents + [UInt64]$LiveDecoderReport.staleResultDrops +
        [UInt64]$LiveDecoderReport.captureStall.count + [UInt64]$LiveDecoderReport.visualStall.count
    if ([UInt64]$LiveDecoderReport.captureArrivedFrames -eq 0 -and [UInt64]$EncoderReport.submittedFrames -gt 0)
    {
        $schedulerCount++
        Add-PBFailureSupport -Support $support -Category scheduler -Evidence 'Encoder submitted frames while live Decoder captured zero frames' -Count 0
    }
    $terminalText = ([string]$LiveDecoderReport.statusMessage + ' ' + [string]$LiveDecoderReport.errorDetail).ToLowerInvariant()
    if ([string]$LiveDecoderReport.state -ne 'Completed' -and
        ($terminalText.Contains('timeout') -or $terminalText.Contains('no progress') -or
        $terminalText.Contains('without descriptor') -or $terminalText.Contains('stopped')))
    {
        $schedulerCount++
        Add-PBFailureSupport -Support $support -Category scheduler -Evidence 'Frozen run/no-progress window ended before Receiver convergence' -Count 0
    }
    if ($schedulerCount -gt 0)
    {
        Add-PBFailureSupport -Support $support -Category scheduler -Evidence 'Live Decoder capture/result queue or stall counters' -Count $schedulerCount
    }
    $result = [ordered]@{}
    foreach ($category in @('geometry', 'signal', 'temporal', 'metric', 'scheduler'))
    {
        $result[$category] = [ordered]@{
            supported = [UInt64]$support[$category].count -gt 0
            count = [UInt64]$support[$category].count
            evidence = @($support[$category].evidence)
        }
    }
    return $result
}

Export-ModuleMember -Function Assert-PBMatrixExactKeys, Assert-PBMatrixIdentityEqual, Get-PBStep21ScaleTarget, `
    New-PBRemoteVisualStep21ExpectedCells, Get-PBRemoteVisualStep21CellRequiredRoi, `
    New-PBRemoteVisualStep21HardwareScopePartition, Import-PBRemoteVisualStep21MatrixSpec, `
    Import-PBRemoteVisualStep21HardwareScope, New-PBRemoteVisualStep21RunLedgerEntry, `
    Import-PBRemoteVisualStep21RunLedger, Import-PBRemoteVisualStep21RunLedgerSeal, `
    Import-PBRemoteVisualStep21ComputerBMonitorCatalog, New-PBRemoteVisualStep21EndpointTopologyValue, `
    Import-PBRemoteVisualStep21EndpointScope, Import-PBRemoteVisualStep21EndpointScopeSeal, `
    Get-PBNonNegativeUInt64, Get-PBRemoteVisualObservedLocatorGeometry, Assert-PBRemoteVisualObservedLocatorTarget, `
    Get-PBRemoteVisualAuthoritativeMetrics, `
    New-PBRemoteVisualMatrixRunRecordValue, `
    Import-PBRemoteVisualMatrixRunRecord, Get-PBRemoteVisualFailureSupport

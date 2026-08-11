[CmdletBinding()]
param(
    [string]$ContourLog = "",
    [string]$OutputPath = "",
    [string]$HistoryPath = "",
    [string]$EvaluatorPath = "",
    [ValidateRange(4, 128)]
    [int]$PopulationSize = 8,
    [ValidateRange(1, 32)]
    [int]$EliteCount = 2,
    [ValidateRange(0.0, 1.0)]
    [double]$MutationRate = 0.25,
    [ValidateRange(0.0, 1.0)]
    [double]$ResetRate = 0.04,
    [ValidateRange(0.001, 1.0)]
    [double]$MutationSigma = 0.12,
    [ValidateRange(1, 86400)]
    [int]$EvaluationTimeoutSeconds = 300,
    [ValidateRange(0.9, 1.0)]
    [double]$TargetCoverage = 0.998,
    [ValidateRange(0.0, 100.0)]
    [double]$MaximumOutwardDistance = 1.0,
    [ValidateRange(0, 2147483647)]
    [int]$RandomSeed = 0,
    [ValidateRange(0, 2147483647)]
    [int]$MaxEvaluations = 0,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$invariantCulture = [Globalization.CultureInfo]::InvariantCulture
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$headerPath = Join-Path $repositoryRoot "src\gui\fill\differential_cover.h"
$gaBuildDirectory = Join-Path $repositoryRoot "build\ga"
$corpusDirectory = Join-Path $gaBuildDirectory "corpus"
$script:activeProcess = $null
$script:bestEvaluation = $null
$script:evaluationCount = 0
$script:generation = 0
$script:stopStatus = "running"
$script:sourceContourLog = $null
$script:evaluatorPath = $null
$script:effectiveRandomSeed = 0
$evaluatorShutdownGraceSeconds = 15.0

function Resolve-WorkspacePath {
    param(
        [string]$Path,
        [string]$Fallback
    )

    $selected = if ([string]::IsNullOrWhiteSpace($Path)) { $Fallback } else { $Path }
    if (-not [IO.Path]::IsPathRooted($selected)) {
        $selected = Join-Path $repositoryRoot $selected
    }

    return [IO.Path]::GetFullPath($selected)
}

$OutputPath = Resolve-WorkspacePath $OutputPath "build\differential_ga_best.json"
$HistoryPath = Resolve-WorkspacePath $HistoryPath "build\differential_ga_history.jsonl"
if ($OutputPath.Equals($HistoryPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath and HistoryPath must be different files"
}

function Get-LatestContourLog {
    if (-not [string]::IsNullOrWhiteSpace($ContourLog)) {
        $resolved = Resolve-WorkspacePath $ContourLog $ContourLog
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "Contour log does not exist: $resolved"
        }

        return Get-Item -LiteralPath $resolved
    }

    $searchDirectories = @(
        (Join-Path $repositoryRoot "build\Release"),
        (Join-Path $repositoryRoot "build\Release\logs")
    )
    $logs = @(
        foreach ($directory in $searchDirectories) {
            if (Test-Path -LiteralPath $directory -PathType Container) {
                Get-ChildItem -LiteralPath $directory -Filter "pen_fill*.log" -File
            }
        }
    )
    if ($logs.Count -eq 0) {
        throw "No pen_fill*.log was found under build\Release or build\Release\logs"
    }

    return $logs | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
}

function Get-NumericConstant {
    param(
        [string]$Source,
        [string]$Name
    )

    $pattern = "inline\s+constexpr\s+(?:int|double)\s+$([regex]::Escape($Name))\s*=\s*([^;]+);"
    $match = [regex]::Match($Source, $pattern)
    if (-not $match.Success) {
        throw "Could not read $Name from $headerPath"
    }

    return [double]::Parse($match.Groups[1].Value.Trim(), $invariantCulture)
}

function New-NumericGene {
    param(
        [string]$Name,
        [string]$Environment,
        [string]$Type,
        [string]$Scale,
        [string]$DefaultConstant,
        [string]$MinimumConstant,
        [string]$MaximumConstant,
        [double]$Offset = 0.0
    )

    return [pscustomobject][ordered]@{
        Name = $Name
        Environment = $Environment
        Type = $Type
        Scale = $Scale
        Default = Get-NumericConstant $script:headerSource $DefaultConstant
        Minimum = Get-NumericConstant $script:headerSource $MinimumConstant
        Maximum = Get-NumericConstant $script:headerSource $MaximumConstant
        Offset = $Offset
    }
}

function New-BooleanGene {
    param(
        [string]$Name,
        [string]$Environment,
        [bool]$Default
    )

    return [pscustomobject][ordered]@{
        Name = $Name
        Environment = $Environment
        Type = "Boolean"
        Scale = "Boolean"
        Default = $Default
        Minimum = $false
        Maximum = $true
        Offset = 0.0
    }
}

$script:headerSource = Get-Content -LiteralPath $headerPath -Raw
$genes = @(
    New-NumericGene "budget" "FLS_CORPUS_BUDGET" "Integer" "Log" "kDefaultBudget" "kMinimumBudget" "kMaximumBudget"
    New-NumericGene "adamIterations" "FLS_CORPUS_ADAM_ITERATIONS" "Integer" "Log" "kDefaultAdamIterations" "kMinimumAdamIterations" "kMaximumAdamIterations"
    New-NumericGene "restarts" "FLS_CORPUS_RESTARTS" "Integer" "LogZero" "kDefaultRestarts" "kMinimumRestarts" "kMaximumRestarts" 1.0
    New-NumericGene "spillWeight" "FLS_CORPUS_SPILL_WEIGHT" "Real" "Log" "kDefaultSpillWeight" "kMinimumSpillWeight" "kMaximumSpillWeight"
    New-NumericGene "epsArea" "FLS_CORPUS_EPS_AREA" "Real" "LogZero" "kDefaultEpsArea" "kMinimumEpsArea" "kMaximumEpsArea" 0.001
    New-NumericGene "epsGain" "FLS_CORPUS_EPS_GAIN" "Real" "Log" "kDefaultEpsGain" "kMinimumEpsGain" "kMaximumEpsGain"
    New-NumericGene "epsSpill" "FLS_CORPUS_EPS_SPILL" "Real" "LogZero" "kDefaultEpsSpill" "kMinimumEpsSpill" "kMaximumEpsSpill" 0.001
    New-NumericGene "adamLearningRate" "FLS_CORPUS_ADAM_LEARNING_RATE" "Real" "Log" "kDefaultAdamLearningRate" "kMinimumAdamLearningRate" "kMaximumAdamLearningRate"
    New-NumericGene "inactivityTimeoutSeconds" "FLS_CORPUS_INACTIVITY_TIMEOUT" "Real" "LogZero" "kDefaultInactivityTimeoutSeconds" "kMinimumInactivityTimeoutSeconds" "kMaximumInactivityTimeoutSeconds" 0.1
    New-NumericGene "boundaryTolerance" "FLS_CORPUS_BOUNDARY_TOLERANCE" "Real" "Log" "kDefaultBoundaryTolerance" "kMinimumBoundaryTolerance" "kMaximumBoundaryTolerance"
    New-NumericGene "outwardMargin" "FLS_CORPUS_OUTWARD_MARGIN" "Real" "LogZero" "kDefaultOutwardMargin" "kMinimumOutwardMargin" "kMaximumOutwardMargin" 0.001
    New-NumericGene "areaWindowRatio" "FLS_CORPUS_AREA_WINDOW_RATIO" "Real" "Log" "kDefaultAreaWindowRatio" "kMinimumAreaWindowRatio" "kMaximumAreaWindowRatio"
    New-NumericGene "targetCoverageRatio" "FLS_CORPUS_TARGET_COVERAGE_RATIO" "Real" "Linear" "kDefaultTargetCoverageRatio" "kMinimumTargetCoverageRatio" "kMaximumTargetCoverageRatio"
    New-NumericGene "tverskyAlpha" "FLS_CORPUS_TVERSKY_ALPHA" "Real" "LogZero" "kDefaultTverskyAlpha" "kMinimumTverskyAlpha" "kMaximumTverskyAlpha" 0.001
    New-NumericGene "tverskyBeta" "FLS_CORPUS_TVERSKY_BETA" "Real" "Log" "kDefaultTverskyBeta" "kMinimumTverskyBeta" "kMaximumTverskyBeta"
    New-NumericGene "featureWeight" "FLS_CORPUS_FEATURE_WEIGHT" "Real" "LogZero" "kDefaultFeatureWeight" "kMinimumFeatureWeight" "kMaximumFeatureWeight" 0.001
    New-NumericGene "featureRestarts" "FLS_CORPUS_FEATURE_RESTARTS" "Integer" "LogZero" "kDefaultFeatureRestarts" "kMinimumFeatureRestarts" "kMaximumFeatureRestarts" 1.0
    [pscustomobject][ordered]@{
        Name = "seed"
        Environment = "FLS_CORPUS_SEED"
        Type = "Integer"
        Scale = "Linear"
        Default = 0.0
        Minimum = Get-NumericConstant $script:headerSource "kMinimumSeed"
        Maximum = Get-NumericConstant $script:headerSource "kMaximumSeed"
        Offset = 0.0
    }
    New-BooleanGene "useRouter" "FLS_CORPUS_USE_ROUTER" $true
    New-BooleanGene "useGpu" "FLS_CORPUS_USE_GPU" $true
    New-BooleanGene "useWeightedContour" "FLS_CORPUS_WEIGHTED_CONTOUR" $true
)

if ($EliteCount -ge $PopulationSize) {
    throw "EliteCount must be smaller than PopulationSize"
}

function Limit-Value {
    param(
        [double]$Value,
        [double]$Minimum,
        [double]$Maximum
    )

    return [Math]::Min($Maximum, [Math]::Max($Minimum, $Value))
}

function ConvertTo-UnitValue {
    param(
        [double]$Value,
        [pscustomobject]$Gene
    )

    if ($Gene.Maximum -le $Gene.Minimum) {
        return 0.0
    }
    switch ($Gene.Scale) {
        "Log" {
            return [Math]::Log($Value / $Gene.Minimum) / [Math]::Log($Gene.Maximum / $Gene.Minimum)
        }
        "LogZero" {
            $minimum = $Gene.Minimum + $Gene.Offset
            $maximum = $Gene.Maximum + $Gene.Offset
            return [Math]::Log(($Value + $Gene.Offset) / $minimum) / [Math]::Log($maximum / $minimum)
        }
        default {
            return ($Value - $Gene.Minimum) / ($Gene.Maximum - $Gene.Minimum)
        }
    }
}

function ConvertFrom-UnitValue {
    param(
        [double]$UnitValue,
        [pscustomobject]$Gene
    )

    $unit = Limit-Value $UnitValue 0.0 1.0
    switch ($Gene.Scale) {
        "Log" {
            $value = $Gene.Minimum * [Math]::Pow($Gene.Maximum / $Gene.Minimum, $unit)
        }
        "LogZero" {
            $minimum = $Gene.Minimum + $Gene.Offset
            $maximum = $Gene.Maximum + $Gene.Offset
            $value = $minimum * [Math]::Pow($maximum / $minimum, $unit) - $Gene.Offset
        }
        default {
            $value = $Gene.Minimum + ($Gene.Maximum - $Gene.Minimum) * $unit
        }
    }
    $value = Limit-Value $value $Gene.Minimum $Gene.Maximum
    if ($Gene.Type -eq "Integer") {
        return [int64][Math]::Round($value)
    }

    return $value
}

function New-DefaultGenome {
    $result = [ordered]@{}
    foreach ($gene in $genes) {
        if ($gene.Type -eq "Boolean") {
            $result[$gene.Name] = [bool]$gene.Default
        } elseif ($gene.Type -eq "Integer") {
            $result[$gene.Name] = [int64]$gene.Default
        } else {
            $result[$gene.Name] = [double]$gene.Default
        }
    }

    return $result
}

function Copy-Genome {
    param([System.Collections.IDictionary]$Genome)

    $result = [ordered]@{}
    foreach ($gene in $genes) {
        $result[$gene.Name] = $Genome[$gene.Name]
    }

    return $result
}

function Get-GaussianSample {
    param([Random]$Random)

    $left = [Math]::Max([double]::Epsilon, $Random.NextDouble())
    $right = $Random.NextDouble()
    return [Math]::Sqrt(-2.0 * [Math]::Log($left)) * [Math]::Cos(2.0 * [Math]::PI * $right)
}

function New-ChildGenome {
    param(
        [System.Collections.IDictionary]$Left,
        [System.Collections.IDictionary]$Right,
        [Random]$Random,
        [double]$AppliedMutationRate = $MutationRate
    )

    $result = [ordered]@{}
    foreach ($gene in $genes) {
        if ($gene.Type -eq "Boolean") {
            $value = if ($Random.NextDouble() -lt 0.5) { $Left[$gene.Name] } else { $Right[$gene.Name] }
            if ($Random.NextDouble() -lt $AppliedMutationRate) {
                $value = -not [bool]$value
            }
            $result[$gene.Name] = [bool]$value
            continue
        }

        $leftUnit = ConvertTo-UnitValue ([double]$Left[$gene.Name]) $gene
        $rightUnit = ConvertTo-UnitValue ([double]$Right[$gene.Name]) $gene
        $selector = $Random.NextDouble()
        if ($selector -lt 0.4) {
            $unit = $leftUnit
        } elseif ($selector -lt 0.8) {
            $unit = $rightUnit
        } else {
            $unit = $leftUnit + ($rightUnit - $leftUnit) * $Random.NextDouble()
        }
        if ($Random.NextDouble() -lt $ResetRate) {
            $unit = $Random.NextDouble()
        } elseif ($Random.NextDouble() -lt $AppliedMutationRate) {
            $unit += (Get-GaussianSample $Random) * $MutationSigma
        }
        $result[$gene.Name] = ConvertFrom-UnitValue $unit $gene
    }

    return $result
}

function Get-GenomeHash {
    param([System.Collections.IDictionary]$Genome)

    $json = $Genome | ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Test-BetterEvaluation {
    param(
        $Left,
        $Right
    )

    if ($null -eq $Right) {
        return $true
    }
    if ([bool]$Left.Valid -ne [bool]$Right.Valid) {
        return [bool]$Left.Valid
    }
    if (-not $Left.Valid) {
        return [double]$Left.ElapsedSeconds -lt [double]$Right.ElapsedSeconds
    }
    if ([bool]$Left.Feasible -ne [bool]$Right.Feasible) {
        return [bool]$Left.Feasible
    }
    if ($Left.Feasible) {
        if ([int]$Left.ShapeCount -ne [int]$Right.ShapeCount) {
            return [int]$Left.ShapeCount -lt [int]$Right.ShapeCount
        }
        if ([Math]::Abs([double]$Left.CoverageRatio - [double]$Right.CoverageRatio) -gt 1e-12) {
            return [double]$Left.CoverageRatio -gt [double]$Right.CoverageRatio
        }
        return [double]$Left.SolverWallSeconds -lt [double]$Right.SolverWallSeconds
    }
    if ([Math]::Abs([double]$Left.ConstraintViolation - [double]$Right.ConstraintViolation) -gt 1e-12) {
        return [double]$Left.ConstraintViolation -lt [double]$Right.ConstraintViolation
    }
    if ([Math]::Abs([double]$Left.CoverageRatio - [double]$Right.CoverageRatio) -gt 1e-12) {
        return [double]$Left.CoverageRatio -gt [double]$Right.CoverageRatio
    }
    if ([int]$Left.ShapeCount -ne [int]$Right.ShapeCount) {
        return [int]$Left.ShapeCount -lt [int]$Right.ShapeCount
    }

    return [double]$Left.SolverWallSeconds -lt [double]$Right.SolverWallSeconds
}

function Sort-Evaluations {
    param([object[]]$Evaluations)

    $result = [Collections.ArrayList]::new()
    foreach ($evaluation in $Evaluations) {
        $index = 0
        while ($index -lt $result.Count -and -not (Test-BetterEvaluation $evaluation $result[$index])) {
            ++$index
        }
        [void]$result.Insert($index, $evaluation)
    }

    return @($result)
}

function Select-TournamentParent {
    param(
        [object[]]$Population,
        [Random]$Random
    )

    $best = $null
    $rounds = [Math]::Min(3, $Population.Count)
    for ($round = 0; $round -lt $rounds; ++$round) {
        $candidate = $Population[$Random.Next(0, $Population.Count)]
        if (Test-BetterEvaluation $candidate $best) {
            $best = $candidate
        }
    }

    return $best
}

function Convert-EnvironmentValue {
    param(
        $Value,
        [pscustomobject]$Gene
    )

    if ($Gene.Type -eq "Boolean") {
        if ([bool]$Value) {
            return "true"
        }
        return "false"
    }
    if ($Gene.Type -eq "Integer") {
        return ([int64]$Value).ToString($invariantCulture)
    }

    return ([double]$Value).ToString("R", $invariantCulture)
}

function Set-GenomeEnvironment {
    param([System.Collections.IDictionary]$Genome)

    foreach ($gene in $genes) {
        [Environment]::SetEnvironmentVariable(
            $gene.Environment,
            (Convert-EnvironmentValue $Genome[$gene.Name] $gene),
            [EnvironmentVariableTarget]::Process)
    }
    [Environment]::SetEnvironmentVariable(
        "FLS_CORPUS_CONFIGURATION",
        "genetic-search",
        [EnvironmentVariableTarget]::Process)
    $solverTimeLimit = [Math]::Max(
        1.0,
        $EvaluationTimeoutSeconds - $evaluatorShutdownGraceSeconds)
    [Environment]::SetEnvironmentVariable(
        "FLS_CORPUS_HARD_TIMEOUT_SECONDS",
        $solverTimeLimit.ToString("R", $invariantCulture),
        [EnvironmentVariableTarget]::Process)
}

function Stop-ActiveEvaluator {
    if ($null -eq $script:activeProcess -or $script:activeProcess.HasExited) {
        return
    }
    try {
        $script:activeProcess.Kill()
        [void]$script:activeProcess.WaitForExit(5000)
    } catch {
    }
}

function Invoke-GenomeEvaluation {
    param(
        [System.Collections.IDictionary]$Genome,
        [string]$GenomeHash
    )

    ++$script:evaluationCount
    Set-GenomeEnvironment $Genome
    $stdoutPath = Join-Path $gaBuildDirectory ("evaluation-{0}.stdout.json" -f $script:evaluationCount)
    $stderrPath = Join-Path $gaBuildDirectory ("evaluation-{0}.stderr.log" -f $script:evaluationCount)
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $processExitCode = -1
    $failure = ""
    try {
        $script:activeProcess = Start-Process -FilePath $script:evaluatorPath `
            -ArgumentList @("--corpus", ('"{0}"' -f $corpusDirectory)) `
            -WorkingDirectory $repositoryRoot `
            -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        while (-not $script:activeProcess.HasExited) {
            if ($timer.Elapsed.TotalSeconds -ge $EvaluationTimeoutSeconds) {
                $timedOut = $true
                Stop-ActiveEvaluator
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if ($script:activeProcess.HasExited) {
            [void]$script:activeProcess.WaitForExit()
            $processExitCode = [int]$script:activeProcess.ExitCode
        }
    } catch {
        $failure = $_.Exception.Message
        Stop-ActiveEvaluator
    } finally {
        $timer.Stop()
        $script:activeProcess = $null
    }

    $resultObject = $null
    if (-not $timedOut -and $processExitCode -eq 0 -and (Test-Path -LiteralPath $stdoutPath)) {
        try {
            $document = Get-Content -LiteralPath $stdoutPath -Raw | ConvertFrom-Json
            $resultObject = @($document.results)[0]
        } catch {
            $failure = "Invalid evaluator output: $($_.Exception.Message)"
        }
    }
    $evaluatorLog = ""
    if (Test-Path -LiteralPath $stderrPath) {
        $stderrText = Get-Content -LiteralPath $stderrPath -Raw
        if ($null -ne $stderrText) {
            $evaluatorLog = $stderrText.Trim()
        }
    }
    if ([string]::IsNullOrWhiteSpace($failure) -and
        ($null -eq $resultObject -or $processExitCode -ne 0)) {
        $failure = $evaluatorLog
    }

    $resultNumbersAreFinite = $false
    if ($null -ne $resultObject) {
        $numericValues = @(
            $resultObject.targetArea,
            $resultObject.missingArea,
            $resultObject.outsideArea,
            $resultObject.maximumOutwardDistance,
            $resultObject.wallSeconds)
        $resultNumbersAreFinite = $true
        foreach ($value in $numericValues) {
            try {
                $number = [double]$value
                if ([double]::IsNaN($number) -or
                    [double]::IsInfinity($number)) {
                    $resultNumbersAreFinite = $false
                    break
                }
            } catch {
                $resultNumbersAreFinite = $false
                break
            }
        }
    }
    $valid = $null -ne $resultObject `
        -and [string]::IsNullOrWhiteSpace([string]$resultObject.error) `
        -and $resultNumbersAreFinite `
        -and [double]$resultObject.targetArea -gt 0.0 `
        -and [int]$resultObject.placements -ge 0
    $targetArea = if ($valid) { [double]$resultObject.targetArea } else { 0.0 }
    $missingArea = if ($valid) { [double]$resultObject.missingArea } else { [double]::PositiveInfinity }
    $coverageRatio = if ($valid -and $targetArea -gt 0.0) {
        Limit-Value (1.0 - $missingArea / $targetArea) 0.0 1.0
    } else {
        0.0
    }
    $outwardDistance = if ($valid) { [double]$resultObject.maximumOutwardDistance } else { [double]::PositiveInfinity }
    $coverageShortfall = [Math]::Max(0.0, $TargetCoverage - $coverageRatio)
    $outwardExcess = [Math]::Max(0.0, $outwardDistance - $MaximumOutwardDistance)
    $constraintViolation = $coverageShortfall + $outwardExcess / [Math]::Max(1.0, $MaximumOutwardDistance)
    $feasible = $valid `
        -and $coverageRatio -ge $TargetCoverage `
        -and $outwardDistance -le $MaximumOutwardDistance + 1e-9
    $hasHardTimeoutFlag = $null -ne $resultObject `
        -and $resultObject.PSObject.Properties.Name -contains "hardTimedOut"

    $evaluation = [pscustomobject][ordered]@{
        Evaluation = $script:evaluationCount
        Generation = $script:generation
        GenomeHash = $GenomeHash
        Options = Copy-Genome $Genome
        Valid = $valid
        Feasible = $feasible
        CoverageRatio = $coverageRatio
        ShapeCount = if ($valid) { [int]$resultObject.placements } else { [int]::MaxValue }
        SolverWallSeconds = if ($valid) { [double]$resultObject.wallSeconds } else { $timer.Elapsed.TotalSeconds }
        ElapsedSeconds = $timer.Elapsed.TotalSeconds
        MissingArea = $missingArea
        OutsideArea = if ($valid) { [double]$resultObject.outsideArea } else { [double]::PositiveInfinity }
        MaximumOutwardDistance = $outwardDistance
        BoundaryFScore = if ($valid) { [double]$resultObject.boundaryFScore } else { 0.0 }
        BoundaryDistance95 = if ($valid) { [double]$resultObject.boundaryDistance95 } else { [double]::PositiveInfinity }
        Tversky = if ($valid) { [double]$resultObject.tversky } else { 0.0 }
        ConstraintViolation = $constraintViolation
        TimedOut = $timedOut
        SolverCancelled = if ($null -ne $resultObject) {
            [bool]$resultObject.cancelled
        } else {
            $false
        }
        SolverTimedOut = if ($null -ne $resultObject) {
            [bool]$resultObject.timedOut
        } else {
            $false
        }
        HardTimedOut = if ($hasHardTimeoutFlag) {
            [bool]$resultObject.hardTimedOut
        } else {
            $false
        }
        ProcessExitCode = $processExitCode
        Failure = $failure
        EvaluatorLog = $evaluatorLog
        EvaluatedUtc = [DateTime]::UtcNow.ToString("o", $invariantCulture)
    }
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

    return $evaluation
}

function Get-PersistedEvaluation {
    param($Evaluation)

    if ($null -eq $Evaluation) {
        return $null
    }

    $finiteOrNull = {
        param([double]$Value)
        if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value)) {
            return $null
        }
        return $Value
    }

    return [ordered]@{
        evaluation = $Evaluation.Evaluation
        generation = $Evaluation.Generation
        genomeHash = $Evaluation.GenomeHash
        feasible = $Evaluation.Feasible
        valid = $Evaluation.Valid
        coverageRatio = $Evaluation.CoverageRatio
        shapeCount = $Evaluation.ShapeCount
        solverWallSeconds = & $finiteOrNull ([double]$Evaluation.SolverWallSeconds)
        elapsedSeconds = & $finiteOrNull ([double]$Evaluation.ElapsedSeconds)
        missingArea = & $finiteOrNull ([double]$Evaluation.MissingArea)
        outsideArea = & $finiteOrNull ([double]$Evaluation.OutsideArea)
        maximumOutwardDistance = & $finiteOrNull ([double]$Evaluation.MaximumOutwardDistance)
        boundaryFScore = & $finiteOrNull ([double]$Evaluation.BoundaryFScore)
        boundaryDistance95 = & $finiteOrNull ([double]$Evaluation.BoundaryDistance95)
        tversky = & $finiteOrNull ([double]$Evaluation.Tversky)
        timedOut = $Evaluation.TimedOut
        solverCancelled = $Evaluation.SolverCancelled
        solverTimedOut = $Evaluation.SolverTimedOut
        hardTimedOut = $Evaluation.HardTimedOut
        processExitCode = $Evaluation.ProcessExitCode
        failure = $Evaluation.Failure
        evaluatorLog = $Evaluation.EvaluatorLog
        evaluatedUtc = $Evaluation.EvaluatedUtc
        parameters = $Evaluation.Options
    }
}

function Write-BestSnapshot {
    param([string]$Status)

    $parameterSpace = @(
        foreach ($gene in $genes) {
            [ordered]@{
                name = $gene.Name
                type = $gene.Type
                scale = $gene.Scale
                minimum = $gene.Minimum
                maximum = $gene.Maximum
                default = $gene.Default
            }
        }
    )
    $snapshot = [ordered]@{
        schemaVersion = 1
        status = $Status
        updatedUtc = [DateTime]::UtcNow.ToString("o", $invariantCulture)
        sourceContourLog = $script:sourceContourLog
        evaluator = $script:evaluatorPath
        evaluationCount = $script:evaluationCount
        generation = $script:generation
        randomSeed = $script:effectiveRandomSeed
        objective = [ordered]@{
            minimumCoverageRatio = $TargetCoverage
            maximumOutwardDistance = $MaximumOutwardDistance
            feasiblePriority = @("shapeCount", "coverageRatio", "solverWallSeconds")
        }
        parameterSpace = $parameterSpace
        best = Get-PersistedEvaluation $script:bestEvaluation
    }
    $directory = Split-Path -Parent $OutputPath
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $temporaryPath = "$OutputPath.tmp"
    $encoding = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText(
        $temporaryPath,
        ($snapshot | ConvertTo-Json -Depth 10),
        $encoding)
    Move-Item -LiteralPath $temporaryPath -Destination $OutputPath -Force
}

function Write-HistoryEvaluation {
    param($Evaluation)

    $directory = Split-Path -Parent $HistoryPath
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $line = (Get-PersistedEvaluation $Evaluation | ConvertTo-Json -Depth 8 -Compress) + [Environment]::NewLine
    [IO.File]::AppendAllText($HistoryPath, $line, [Text.UTF8Encoding]::new($false))
}

function Ensure-Evaluator {
    if (-not [string]::IsNullOrWhiteSpace($EvaluatorPath)) {
        $resolved = Resolve-WorkspacePath $EvaluatorPath $EvaluatorPath
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "Evaluator does not exist: $resolved"
        }
        $script:evaluatorPath = $resolved
        return
    }

    $script:evaluatorPath = Join-Path $gaBuildDirectory "Release\fls_differential_cover_tests.exe"
    if ($SkipBuild) {
        if (-not (Test-Path -LiteralPath $script:evaluatorPath -PathType Leaf)) {
            throw "Evaluator does not exist and -SkipBuild was specified: $script:evaluatorPath"
        }
        return
    }

    if (-not $env:VCPKG_ROOT) {
        if (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake") {
            $env:VCPKG_ROOT = "C:\vcpkg"
        } else {
            $env:VCPKG_ROOT = "C:\vcpkg\vcpkg"
        }
    }
    $toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
        throw "vcpkg toolchain file not found: $toolchain"
    }
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:VCPKG_ROOT "installed\x64-windows\Qt6\plugins\platforms"
    & cmake -S $repositoryRoot -B $gaBuildDirectory `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        -DVCPKG_TARGET_TRIPLET=x64-windows `
        -DFLS_PRIVACY_POLICY=ON `
        -DFLS_BUILD_HELPER_TOOLS=OFF `
        -DFLS_BUILD_LIVERY_COMPARE=OFF `
        -DFLS_ENABLE_IMGGEN_MENU=OFF `
        -DFLS_ENABLE_CUDA=OFF `
        -DENFORCE_SHAPE_LIMITS=ON `
        -DFLS_BUILD_TESTS=ON
    if ($LASTEXITCODE -ne 0) {
        throw "GA evaluator configuration failed with exit code $LASTEXITCODE"
    }
    & cmake --build $gaBuildDirectory --config Release `
        --target fls_differential_cover_tests --parallel 4
    if ($LASTEXITCODE -ne 0) {
        throw "GA evaluator build failed with exit code $LASTEXITCODE"
    }
}

$environmentNames = @($genes.Environment) + @(
    "FLS_CORPUS_CONFIGURATION",
    "FLS_CORPUS_HARD_TIMEOUT_SECONDS")
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable(
        $name, [EnvironmentVariableTarget]::Process)
}

$random = $null
$population = @()
$seenGenomes = [Collections.Generic.HashSet[string]]::new()
try {
    $latestLog = Get-LatestContourLog
    $script:sourceContourLog = $latestLog.FullName
    [IO.Directory]::CreateDirectory($corpusDirectory) | Out-Null
    Copy-Item -LiteralPath $latestLog.FullName `
        -Destination (Join-Path $corpusDirectory "pen_fill.log") -Force
    Ensure-Evaluator

    $script:effectiveRandomSeed = if ($RandomSeed -eq 0) {
        [int]([DateTime]::UtcNow.Ticks % 2147483647)
    } else {
        $RandomSeed
    }
    $random = [Random]::new($script:effectiveRandomSeed)
    Remove-Item -LiteralPath $HistoryPath -Force -ErrorAction SilentlyContinue
    Write-BestSnapshot "running"

    $defaultGenome = New-DefaultGenome
    $initialGenomes = [Collections.ArrayList]::new()
    [void]$initialGenomes.Add($defaultGenome)
    [void]$seenGenomes.Add((Get-GenomeHash $defaultGenome))
    $goalGenome = Copy-Genome $defaultGenome
    $goalGenome["targetCoverageRatio"] = [Math]::Max(
        [double]$goalGenome["targetCoverageRatio"], $TargetCoverage)
    [void]$initialGenomes.Add($goalGenome)
    [void]$seenGenomes.Add((Get-GenomeHash $goalGenome))
    while ($initialGenomes.Count -lt $PopulationSize) {
        $candidate = New-ChildGenome $defaultGenome $goalGenome $random 0.5
        $hash = Get-GenomeHash $candidate
        if ($seenGenomes.Add($hash)) {
            [void]$initialGenomes.Add($candidate)
        }
    }

    Write-Host "Differential-cover genetic search"
    Write-Host "Contour: $($latestLog.FullName)"
    Write-Host "Best parameters: $OutputPath"
    Write-Host "History: $HistoryPath"
    Write-Host "Press Ctrl+C to stop; the best completed evaluation is already durable."

    while ($true) {
        $genomes = if ($script:generation -eq 0) {
            @($initialGenomes)
        } else {
            $children = [Collections.ArrayList]::new()
            for ($eliteIndex = 0; $eliteIndex -lt $EliteCount; ++$eliteIndex) {
                $eliteChild = New-ChildGenome `
                    $population[$eliteIndex].Options `
                    $population[$eliteIndex].Options `
                    $random
                $eliteHash = Get-GenomeHash $eliteChild
                if ($seenGenomes.Add($eliteHash)) {
                    [void]$children.Add($eliteChild)
                }
            }
            while ($children.Count -lt $PopulationSize) {
                $left = Select-TournamentParent $population $random
                $right = Select-TournamentParent $population $random
                $child = New-ChildGenome $left.Options $right.Options $random
                $hash = Get-GenomeHash $child
                if ($seenGenomes.Add($hash)) {
                    [void]$children.Add($child)
                }
            }
            @($children)
        }

        $generationEvaluations = [Collections.ArrayList]::new()
        foreach ($genome in $genomes) {
            if ($MaxEvaluations -gt 0 -and $script:evaluationCount -ge $MaxEvaluations) {
                break
            }
            $hash = Get-GenomeHash $genome
            [void]$seenGenomes.Add($hash)
            $evaluation = Invoke-GenomeEvaluation $genome $hash
            [void]$generationEvaluations.Add($evaluation)
            Write-HistoryEvaluation $evaluation
            $isNewBest = Test-BetterEvaluation $evaluation $script:bestEvaluation
            if ($isNewBest) {
                $script:bestEvaluation = $evaluation
            }
            Write-BestSnapshot "running"

            $coveragePercent = 100.0 * $evaluation.CoverageRatio
            $marker = if ($isNewBest) { " BEST" } else { "" }
            $validText = if (-not $evaluation.Valid) {
                if ($evaluation.TimedOut) { " watchdog-timeout" } else { " invalid" }
            } elseif ($evaluation.HardTimedOut -or
                      $evaluation.SolverTimedOut) {
                " partial-timeout"
            } else {
                ""
            }
            Write-Host ("[{0,5}] generation {1,4}: {2,7:N3}%  {3,5} shapes  {4,8:N2}s{5}{6}" -f `
                $evaluation.Evaluation,
                $evaluation.Generation,
                $coveragePercent,
                $evaluation.ShapeCount,
                $evaluation.SolverWallSeconds,
                $marker,
                $validText)
        }
        if ($generationEvaluations.Count -gt 0) {
            $population = Sort-Evaluations (@($population) + @($generationEvaluations)) |
                Select-Object -First $PopulationSize
        }
        if ($MaxEvaluations -gt 0 -and $script:evaluationCount -ge $MaxEvaluations) {
            $script:stopStatus = "completed"
            break
        }
        ++$script:generation
    }
} catch [System.Management.Automation.PipelineStoppedException] {
    $script:stopStatus = "cancelled"
} catch {
    $script:stopStatus = "failed"
    throw
} finally {
    Stop-ActiveEvaluator
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name,
            $savedEnvironment[$name],
            [EnvironmentVariableTarget]::Process)
    }
    if ($null -ne $script:sourceContourLog -and $null -ne $script:evaluatorPath) {
        Write-BestSnapshot $script:stopStatus
    }
    if ($null -ne $script:bestEvaluation) {
        Write-Host ("Best: {0:N3}% coverage, {1} shapes, {2:N2}s" -f `
            (100.0 * $script:bestEvaluation.CoverageRatio),
            $script:bestEvaluation.ShapeCount,
            $script:bestEvaluation.SolverWallSeconds)
        Write-Host "Parameters saved to $OutputPath"
    }
}

param(
    [string]$HistoryPath = "build/differential_ga_history.jsonl",
    [string]$ContourLogPath = "build/Release/pen_fill.log",
    [string]$EvaluatorPath = "build/ga/Release/fls_differential_cover_tests.exe",
    [string]$OutputPath = "build/ga_visual/ga_fill_comparison.3so"
)

$ErrorActionPreference = "Stop"
$culture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot

function Resolve-WorkspacePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $root $Path))
}

$historyFile = Resolve-WorkspacePath $HistoryPath
$contourLog = Resolve-WorkspacePath $ContourLogPath
$evaluator = Resolve-WorkspacePath $EvaluatorPath
$output = Resolve-WorkspacePath $OutputPath
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) |
    Out-Null

$history = Get-Content -LiteralPath $historyFile |
    ForEach-Object { $_ | ConvertFrom-Json }
$environmentByParameter = [ordered]@{
    budget = "FLS_CORPUS_BUDGET"
    adamIterations = "FLS_CORPUS_ADAM_ITERATIONS"
    restarts = "FLS_CORPUS_RESTARTS"
    spillWeight = "FLS_CORPUS_SPILL_WEIGHT"
    epsArea = "FLS_CORPUS_EPS_AREA"
    epsGain = "FLS_CORPUS_EPS_GAIN"
    epsSpill = "FLS_CORPUS_EPS_SPILL"
    adamLearningRate = "FLS_CORPUS_ADAM_LEARNING_RATE"
    inactivityTimeoutSeconds = "FLS_CORPUS_INACTIVITY_TIMEOUT"
    boundaryTolerance = "FLS_CORPUS_BOUNDARY_TOLERANCE"
    outwardMargin = "FLS_CORPUS_OUTWARD_MARGIN"
    areaWindowRatio = "FLS_CORPUS_AREA_WINDOW_RATIO"
    targetCoverageRatio = "FLS_CORPUS_TARGET_COVERAGE_RATIO"
    tverskyAlpha = "FLS_CORPUS_TVERSKY_ALPHA"
    tverskyBeta = "FLS_CORPUS_TVERSKY_BETA"
    featureWeight = "FLS_CORPUS_FEATURE_WEIGHT"
    featureRestarts = "FLS_CORPUS_FEATURE_RESTARTS"
    seed = "FLS_CORPUS_SEED"
    useRouter = "FLS_CORPUS_USE_ROUTER"
    useGpu = "FLS_CORPUS_USE_GPU"
    useWeightedContour = "FLS_CORPUS_WEIGHTED_CONTOUR"
}
$comparisons = @(
    @{ Evaluation = 1; Label = "01 Defaults" }
    @{ Evaluation = 105; Label = "02 Best boundary" }
    @{ Evaluation = 126; Label = "03 Fast 29 shapes" }
    @{ Evaluation = 132; Label = "04 Strict winner" }
    @{ Evaluation = 178; Label = "05 Near-miss 28 shapes" }
)

$first = $true
foreach ($comparison in $comparisons) {
    $record = $history |
        Where-Object evaluation -eq $comparison.Evaluation |
        Select-Object -First 1
    if ($null -eq $record) {
        throw "Evaluation $($comparison.Evaluation) is absent from $historyFile"
    }
    foreach ($entry in $environmentByParameter.GetEnumerator()) {
        $value = $record.parameters.($entry.Key)
        if ($value -is [bool]) {
            $text = if ($value) { "1" } else { "0" }
        } elseif ($value -is [double] -or $value -is [single] -or
                  $value -is [decimal]) {
            $text = [string]::Format($culture, "{0:R}", [double]$value)
        } else {
            $text = [string]$value
        }
        [Environment]::SetEnvironmentVariable($entry.Value, $text, "Process")
    }
    $env:FLS_EXPORT_APPEND = if ($first) { "0" } else { "1" }
    $env:FLS_EXPORT_VISIBLE = if ($comparison.Evaluation -eq 132) {
        "1"
    } else {
        "0"
    }
    Write-Host "Replaying evaluation $($comparison.Evaluation)..."
    & $evaluator --export-project $contourLog $output $comparison.Label
    if ($LASTEXITCODE -ne 0) {
        throw "Evaluation $($comparison.Evaluation) export failed."
    }
    $first = $false
}

Get-Item -LiteralPath $output

param(
    [string]$AssetsDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryDirectory = Split-Path -Parent $PSScriptRoot
$configurationSource = Join-Path $repositoryDirectory "assets\compact_fit_shapes.json"
if ([string]::IsNullOrWhiteSpace($AssetsDirectory)) {
    $AssetsDirectory = Join-Path $repositoryDirectory "build\Release\assets"
}
if (-not (Test-Path -LiteralPath $AssetsDirectory -PathType Container)) {
    throw "The runtime assets directory is missing: $AssetsDirectory. Build the application once first."
}
$configurationDestination = Join-Path $AssetsDirectory "compact_fit_shapes.json"
$configuration = Get-Content -LiteralPath $configurationSource -Raw | ConvertFrom-Json
if ($configuration.schema_version -ne 1 -or $null -eq $configuration.tasks) {
    throw "The source Compact Fit configuration has an unsupported schema."
}
Copy-Item -LiteralPath $configurationSource -Destination $configurationDestination -Force
Write-Output "Updated $configurationDestination. Compact Fit reads it at the start of the next fit."

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = "C:\vcpkg\vcpkg"
}

Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:VCPKG_ROOT "installed\x64-windows\Qt6\plugins\platforms"

$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot "build\Release\ForzaLiveryStudio.exe"
if (-not (Test-Path $exe)) {
    & (Join-Path $PSScriptRoot "build.ps1")
}

& $exe

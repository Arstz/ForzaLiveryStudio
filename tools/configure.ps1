Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $env:VCPKG_ROOT) {
    if (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        $env:VCPKG_ROOT = "C:\vcpkg\vcpkg"
    }
}

$toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain file not found: $toolchain"
}

Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:VCPKG_ROOT "installed\x64-windows\Qt6\plugins\platforms"

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    cmake -S . -B build `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
        -DVCPKG_TARGET_TRIPLET=x64-windows
} finally {
    Pop-Location
}

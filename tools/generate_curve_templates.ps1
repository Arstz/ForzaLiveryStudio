param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $env:VCPKG_ROOT) {
    if (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        $env:VCPKG_ROOT = "C:\vcpkg\vcpkg"
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$generatorBuild = Join-Path $repoRoot "build\curve-template-generator"
$toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
$qtBin = Join-Path $env:VCPKG_ROOT "installed\x64-windows\bin"
$previousPath = $env:PATH

Push-Location $repoRoot
try {
    cmake -S . -B $generatorBuild `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
        -DVCPKG_TARGET_TRIPLET=x64-windows `
        -DFLS_ENABLE_CUDA=OFF `
        -DFLS_BUILD_HELPER_TOOLS=ON `
        -DFLS_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) {
        throw "Curve template generator configuration failed"
    }
    cmake --build $generatorBuild --config Release `
        --target fls_curve_template_generator --parallel 4
    if ($LASTEXITCODE -ne 0) {
        throw "Curve template generator build failed"
    }
    $env:PATH = "$qtBin;$previousPath"
    $generator = Join-Path $generatorBuild `
        "Release\fls_curve_template_generator.exe"
    $arguments = @("--assets", (Join-Path $repoRoot "assets"))
    if ($Force) {
        $arguments += "--force"
    }
    & $generator @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Curve template generation failed"
    }
} finally {
    $env:PATH = $previousPath
    Pop-Location
}

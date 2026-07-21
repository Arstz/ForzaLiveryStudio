param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$BuildOptions = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$parallelThreads = 4
if ($BuildOptions.Count -gt 0) {
    if ($BuildOptions.Count -ne 2 -or $BuildOptions[0] -ne "--parrallel") {
        throw "Usage: build.ps1 [--parrallel threads]"
    }
    $parsedThreads = 0
    if (-not [int]::TryParse($BuildOptions[1], [ref]$parsedThreads) -or $parsedThreads -lt 1) {
        throw "Parallel thread count must be a positive integer"
    }
    $parallelThreads = $parsedThreads
}

if (-not $env:VCPKG_ROOT) {
    if (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        $env:VCPKG_ROOT = "C:\vcpkg\vcpkg"
    }
}

Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:VCPKG_ROOT "installed\x64-windows\Qt6\plugins\platforms"

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    $buildDir = Join-Path $repoRoot "build"
    $cachePath = Join-Path $buildDir "CMakeCache.txt"

    if (Test-Path $cachePath) {
        $homeLine = Get-Content $cachePath | Where-Object { $_ -like "CMAKE_HOME_DIRECTORY:INTERNAL=*" } | Select-Object -First 1
        if ($homeLine) {
            $cachedSource = ($homeLine -split "=", 2)[1].Replace("\", "/").TrimEnd("/")
            $currentSource = (Resolve-Path $repoRoot).Path.Replace("\", "/").TrimEnd("/")
            if ($cachedSource -ne $currentSource) {
                Remove-Item -LiteralPath $buildDir -Recurse -Force
            }
        }
    }

    if (-not (Test-Path "build\CMakeCache.txt")) {
        & (Join-Path $PSScriptRoot "configure.ps1")
    }

    cmake --build build --config Release --parallel $parallelThreads
} finally {
    Pop-Location
}

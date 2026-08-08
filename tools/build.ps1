param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$BuildOptions = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$buildProgressLineWidth = 99
try {
    $buildProgressLineWidth = [Math]::Max(72, $Host.UI.RawUI.WindowSize.Width - 1)
} catch {
}
$buildProgressActive = $false

function Write-BuildProgress {
    param(
        [int]$Percent,
        [string]$Status
    )

    $barWidth = 40
    $filledWidth = [Math]::Floor($Percent * $barWidth / 100)
    $bar = ("#" * $filledWidth) + ("-" * ($barWidth - $filledWidth))
    $prefix = "Build [$bar] $($Percent.ToString().PadLeft(3))% "
    $statusWidth = [Math]::Max(0, $buildProgressLineWidth - $prefix.Length)
    if ($Status.Length -gt $statusWidth) {
        $Status = $Status.Substring(0, $statusWidth)
    }
    $line = ($prefix + $Status).PadRight($buildProgressLineWidth)
    Write-Host -NoNewline "`r$line"
    $script:buildProgressActive = $true
}

function Write-BuildLogLine {
    param(
        [object]$Line,
        [int]$Percent,
        [string]$Status
    )

    Write-Host -NoNewline ("`r" + (" " * $buildProgressLineWidth) + "`r")
    Write-Host $Line.ToString()
    Write-BuildProgress -Percent $Percent -Status $Status
}

$parallelThreads = 4
Write-BuildProgress -Percent 0 -Status "Validating build options"
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

    Write-BuildProgress -Percent 0 -Status "Configuring CMake"
    $ErrorActionPreference = "Continue"
    & (Join-Path $PSScriptRoot "configure.ps1") 2>&1 | ForEach-Object {
        Write-BuildLogLine -Line $_ -Percent 0 -Status "Configuring CMake"
    }
    $configureExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($configureExitCode -ne 0) {
        throw "CMake configuration failed with exit code $configureExitCode"
    }

    $cppCompileItems = @(
        Get-ChildItem -LiteralPath $buildDir -Filter "*.vcxproj" -File |
            ForEach-Object {
                $projectName = $_.Name
                Select-String -LiteralPath $_.FullName `
                    -Pattern '<ClCompile Include="([^"]+\.cpp)"' -AllMatches |
                    ForEach-Object {
                        foreach ($match in $_.Matches) {
                            $sourcePath = $match.Groups[1].Value
                            if ($sourcePath -notmatch `
                                'mocs_compilation_(Debug|MinSizeRel|RelWithDebInfo)\.cpp$') {
                                "$projectName|$sourcePath"
                            }
                        }
                    }
            } |
            Sort-Object -Unique
    )
    $cppFileTotal = [Math]::Max(1, $cppCompileItems.Count)
    $compiledCppCount = 0
    $buildPercent = 0
    $buildStatus = "Compiled $compiledCppCount/$cppFileTotal C++ files"
    Write-BuildProgress -Percent $buildPercent -Status $buildStatus
    $ErrorActionPreference = "Continue"
    cmake --build build --config Release --parallel $parallelThreads 2>&1 | ForEach-Object {
        $line = $_.ToString()
        if ($line -match '^\s+.*\.cpp\s*$') {
            $compiledCppCount = [Math]::Min(
                $cppFileTotal, $compiledCppCount + 1)
            $buildPercent = [Math]::Floor(
                100 * $compiledCppCount / $cppFileTotal)
            $buildStatus = "Compiled $compiledCppCount/$cppFileTotal C++ files"
        }
        Write-BuildLogLine -Line $_ -Percent $buildPercent -Status $buildStatus
    }
    $buildExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($buildExitCode -ne 0) {
        throw "Build failed with exit code $buildExitCode"
    }
    Write-BuildProgress -Percent 100 -Status "Build complete"
    Write-Host
    $buildProgressActive = $false
} finally {
    if ($buildProgressActive) {
        Write-Host
    }
    Pop-Location
}

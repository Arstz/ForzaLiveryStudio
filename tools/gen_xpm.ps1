# Regenerate assets/*.xpm from the source PNGs. Thin wrapper over gen_xpm.py
# so it fits alongside configure.ps1 / build.ps1 / run.ps1.
#
#   .\gen_xpm.ps1            # rebuild every PNG that already has an .xpm
#   .\gen_xpm.ps1 --all      # also create .xpm for new PNGs without one
#   .\gen_xpm.ps1 --check    # report stale xpm files (exit 1 if any), no writes
$ErrorActionPreference = "Stop"

$python = (Get-Command python -ErrorAction SilentlyContinue)
if ($null -eq $python) {
    $python = (Get-Command py -ErrorAction SilentlyContinue)
}
if ($null -eq $python) {
    Write-Error "Python was not found on PATH. Install Python 3 (with Pillow: python -m pip install pillow)."
    exit 1
}

$script = Join-Path $PSScriptRoot "gen_xpm.py"
& $python.Source $script @args
exit $LASTEXITCODE

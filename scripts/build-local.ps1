# PowerShell wrapper for the local-only MSYS2 build.
$ErrorActionPreference = "Stop"
$bash = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    throw "MSYS2 bash not found at $bash. Install MSYS2 or open its UCRT64 shell."
}
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$msysRoot = $root -replace '\\', '/'
$drive = $msysRoot.Substring(0, 1).ToLower()
$inside = "/$drive" + $msysRoot.Substring(2)
& $bash -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd '$inside' && bash scripts/build-local.sh"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

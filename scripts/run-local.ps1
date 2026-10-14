# PowerShell wrapper for running the locally built ISO in QEMU.
$ErrorActionPreference = "Stop"
$bash = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    throw "MSYS2 bash not found at $bash. Install MSYS2 or open its UCRT64 shell."
}
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$msysRoot = $root -replace '\\', '/'
$drive = $msysRoot.Substring(0, 1).ToLower()
$inside = "/$drive" + $msysRoot.Substring(2)
Write-Host "Starting ChocoOS in QEMU..." -ForegroundColor Cyan
Write-Host "BIOS fallback (instale edk2-ovmf p/ UEFI real)" -ForegroundColor Yellow
Write-Host ""
& $bash -lc "export PATH=/ucrt64/bin:/usr/bin:/$PATH; cd '$inside' && bash scripts/run.sh"
Write-Host ""
Write-Host "QEMU finalizado. Pressione ENTER para sair..." -ForegroundColor Yellow
$null = $host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
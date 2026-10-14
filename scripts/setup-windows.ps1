#Requires -Version 5.1
# ChocoOS - Windows setup (no admin needed, but recommended)
# Installs: MSYS2 (via winget) + toolchain + QEMU
$ErrorActionPreference = "Stop"

Write-Host "=== ChocoOS setup Windows ===" -ForegroundColor Cyan

# 1. MSYS2 via winget
if (-not (Get-Command msys2 -ErrorAction SilentlyContinue)) {
  Write-Host "[1/3] Installing MSYS2 via winget..." -ForegroundColor Yellow
  winget install -e --id MSYS2.MSYS2 --accept-source-agreements --accept-package-agreements
  $msys = "C:\msys64\usr\bin\bash.exe"
} else {
  $msys = "C:\msys64\usr\bin\bash.exe"
}
if (-not (Test-Path $msys)) { $msys = "C:\msys64\usr\bin\bash.exe" }

Write-Host "[2/3] Installing MSYS2 UCRT64 packages (takes a while)..." -ForegroundColor Yellow
& $msys -lc "pacman -Sy --noconfirm && pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-qemu mingw-w64-ucrt-x86_64-python xorriso git make"

Write-Host "[3/3] Checking..." -ForegroundColor Yellow
& $msys -lc "which x86_64-elf-g++ || which clang; which cmake; which nasm; which qemu-system-x86_64; which xorriso; which ld.lld || true"

Write-Host ""
Write-Host "Done! Close and reopen the terminal, then run in MSYS2 UCRT64:" -ForegroundColor Green
Write-Host "  cd /d/Downloads/ChocoOS  (adjust the path)" -ForegroundColor White
Write-Host "  bash scripts/build.sh && bash scripts/run.sh" -ForegroundColor White

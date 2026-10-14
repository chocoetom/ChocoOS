#!/usr/bin/env bash
# Build ChocoOS using only locally installed tools. Never downloads packages
# or bootloader files.
set -euo pipefail
cd "$(dirname "$0")/.."

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "[error] missing local dependency: $1" >&2
    exit 1
  }
}

need cmake
need ninja
need nasm
need clang
need ld.lld
need xorriso
need python3

if [ ! -f build/limine/limine-bios-cd.bin ] ||
   [ ! -f build/limine/BOOTX64.EFI ]; then
  echo "[error] local Limine files not found in build/limine" >&2
  echo "        Copy/build Limine there, then run this script again." >&2
  exit 1
fi

echo "[assets] converting local assets..."
python3 scripts/build-assets.py
python3 scripts/build-font.py

echo "[cmake] configuring..."
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake \
  -DCMAKE_BUILD_TYPE=Release

echo "[build] compiling kernel..."
cmake --build build

echo "[user] building local userland..."
bash scripts/build-user.sh

# Download and stage extra static binaries (micro, lua, micropython)
# NOT NEEDED - compiled directly via build_one in build-user.sh
# if [ -f scripts/fetch-extra.sh ]; then
#   bash scripts/fetch-extra.sh
# fi

echo "[iso] creating local ISO..."
bash scripts/make-iso.sh
echo "OK -> build/ChocoOS.iso"

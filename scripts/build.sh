#!/usr/bin/env bash
# ChocoOS build (run inside MSYS2 UCRT64)
set -e
cd "$(dirname "$0")/.."

LIMINE_VER="v12.9.3"
if [ ! -d build/limine ]; then
  echo "[fetch] Limine $LIMINE_VER ..."
  mkdir -p build
  curl -L "https://github.com/limine-bootloader/limine/releases/download/${LIMINE_VER}/limine-binary.tar.gz" -o build/limine-binary.tar.gz
  mkdir -p build/limine && tar -xzf build/limine-binary.tar.gz -C build/limine --strip-components=1
fi

echo "[assets] converting icons/ ..."
/c/Python314/python.exe scripts/build-assets.py || python3 scripts/build-assets.py || echo "[assets] converter skipped (fallbacks in use)"

echo "[font] baking the Inter UI font ..."
/c/Python314/python.exe scripts/build-font.py || python3 scripts/build-font.py || echo "[font] bake skipped (keeping user/lib/font_ui.h)"

echo "[cmake] configuring..."
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake -DCMAKE_BUILD_TYPE=Release

echo "[build] compiling kernel..."
cmake --build build

echo "[user] building user programs + initrd..."
bash scripts/build-user.sh

echo "[fatdisk] building FAT32 ramdisk..."
bash scripts/make-fatdisk.sh

echo "[iso] building UEFI+BIOS ISO..."
bash scripts/make-iso.sh
echo "OK -> build/ChocoOS.iso"

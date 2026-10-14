#!/usr/bin/env bash
# Monta build/ChocoOS.iso a partir de build/boot/chocoos.elf
set -e
cd "$(dirname "$0")/.."

rm -rf iso_root && mkdir -p iso_root/boot
cp build/boot/chocoos.elf iso_root/boot/
cp boot/limine.cfg iso_root/
# Limine v9+ reads limine.conf (keep .cfg too for older tooling).
cp boot/limine.cfg iso_root/limine.conf
if [ -f build/initrd.tar ]; then
  cp build/initrd.tar iso_root/
  echo "initrd.tar staged"
fi
if [ -f build/fat.img ]; then
  cp build/fat.img iso_root/
  echo "fat.img staged"
else
  echo "fat.img missing (run scripts/make-fatdisk.sh): /disk will be empty"
fi
cp build/limine/limine-bios.sys build/limine/limine-bios-cd.bin \
   build/limine/limine-uefi-cd.bin iso_root/ 2>/dev/null || true
cp build/limine/BOOTX64.EFI iso_root/ 2>/dev/null || true
mkdir -p iso_root/EFI/BOOT
cp build/limine/BOOTX64.EFI iso_root/EFI/BOOT/ 2>/dev/null || cp build/limine/*.EFI iso_root/EFI/BOOT/ 2>/dev/null || true

xorriso -as mkisofs -b limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-uefi-cd.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  iso_root -o build/ChocoOS.iso 2>&1 | tail -n 5

# instala stage BIOS (pula se limine.exe nao rodar no msys — UEFI funciona sem isso no QEMU)
if [ -f build/limine/limine ]; then
  ./build/limine/limine bios-install build/ChocoOS.iso 2>&1 | tail -n 3 || echo "(bios-install pulado, UEFI OK)"
fi

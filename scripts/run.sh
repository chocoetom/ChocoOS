#!/usr/bin/env bash
# Roda ChocoOS no QEMU (UEFI OVMF se disponivel, senao BIOS)
# Modo headless com output para serial.log

cd "$(dirname "$0")/.."
ISO=build/ChocoOS.iso

# Persistent writable volume: a 32 MiB FAT32 image on the secondary IDE
# master (0x170), matching kernel/proc/fat32.cpp. Created via mtools when
# available; otherwise the old raw COLOVOL image is kept as-is.
#
# (Re)create it when missing OR unusable: an earlier run without mtools left a
# zero-filled legacy image, and a stale file used to be kept forever, so /disk
# mounted read-only and every create/mkdir silently failed.
VOL=build/chocovol.img
need_vol=0
[ -f "$VOL" ] || need_vol=1
if [ -f "$VOL" ]; then
  sz=$(stat -c %s "$VOL" 2>/dev/null || echo 0)
  [ "$sz" -lt 4194304 ] && need_vol=1   # < 4 MiB can't be a usable FAT32 volume
  sig=$(dd if="$VOL" bs=1 skip=510 count=2 status=none 2>/dev/null | od -An -tx1 | tr -d ' \n')
  [ "$sig" != "55aa" ] && need_vol=1    # no boot signature => unformatted
fi
if [ "$need_vol" = 1 ]; then
  if command -v mformat >/dev/null 2>&1; then
    # mtools formats the raw image in place (seekable host file).
    dd if=/dev/zero of="$VOL" bs=512 count=65536 status=none
    mformat -i "$VOL" -F -v CHOCOVOL :: 2>/dev/null
    mmd -i "$VOL" ::docs 2>/dev/null || true
    echo "[run] created $VOL (32 MiB FAT32 persistent volume)"
  else
    echo "[run] WARNING: $VOL missing/unformatted and mtools absent -> /disk is READ-ONLY."
    echo "      Install mtools (pacman -S mtools) for a writable /disk."
  fi
fi

# Persistent FAT32 volume on the primary master (0x1F0) — the usual PC spot.
# The driver greps all four legacy IDE positions, so the CD-ROM can stay on
# the secondary master, where -cdrom puts it by default.
VOLARGS="-drive file=$VOL,format=raw,if=none,id=hdrvol -device ide-hd,bus=ide.0,unit=0,drive=hdrvol"
CDARGS="-cdrom $ISO"
# User-mode networking with a virtio NIC (10.0.2.15 guest, 10.0.2.2 host) so the
# kernel network stack is active by default. ioeventfd=off avoids a vCPU stall
# on this Windows QEMU build when the backend has no peer.
NETARGS="-netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off"

# Check for OVMF (UEFI) or fallback to BIOS
if [ -f /usr/share/ovmf/x64/OVMF.fd ] || [ -f /ucrt64/share/ovmf/x64/OVMF.fd ]; then
  OVMF=$(ls /usr/share/ovmf/x64/OVMF.fd /ucrt64/share/ovmf/x64/OVMF.fd 2>/dev/null | head -n1)
  echo "UEFI: $OVMF"
  qemu-system-x86_64 -bios "$OVMF" -m 1024M -display gtk -serial stdio $VOLARGS $CDARGS $NETARGS
else
  echo "BIOS (instale edk2-ovmf p/ UEFI real)"
  echo "Starting QEMU with display and serial stdio..."
  qemu-system-x86_64 -m 1024M -display gtk -serial stdio -boot order=d $VOLARGS $CDARGS $NETARGS
fi
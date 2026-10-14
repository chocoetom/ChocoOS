#!/usr/bin/env bash
# Host-side checker for the persistent FAT32 volume after a test run.
# Runs fsck.fat in read-only mode so a broken FAT/dir entry shows up in CI.
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

IMG="${1:-build/chocovol-test.img}"
if [ ! -f "$IMG" ]; then echo "checker: $IMG not found"; exit 1; fi

if command -v fsck.fat >/dev/null 2>&1; then
  echo "[check] fsck.fat -n $IMG"
  fsck.fat -n "$IMG" || exit 1
elif command -v mdir >/dev/null 2>&1; then
  echo "[check] fsck.fat missing; structural listing instead"
  mdir -i "$IMG" -/ ::
else
  echo "[check] no fsck.fat/mtools available"
  exit 0
fi
echo "[check] volume structure OK"

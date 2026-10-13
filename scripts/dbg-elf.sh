#!/usr/bin/env bash
# Check ELF freshness + rebuild ISO.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin
echo "--- strings check ---"
/ucrt64/bin/strings build/boot/chocoos.elf > build/elf-strings.txt
grep -c "netpoke" build/elf-strings.txt || echo "netpoke: MISSING"
grep -c "txq:" build/elf-strings.txt || echo "txq: MISSING"
grep -c "tr: desc=" build/elf-strings.txt || echo "trdesc: MISSING"
grep -c "cap type" build/elf-strings.txt || echo "captype: MISSING"
echo "--- iso ---"
bash scripts/make-iso.sh
echo "iso exit: $?"
ls -la build/ChocoOS.iso

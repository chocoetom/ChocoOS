#!/usr/bin/env bash
# Headless E2E: LARGE-file (>64KB, multi-cluster) FAT32 persistence.
# Copies a 77568-byte initrd binary to /disk, reboots, and checks it survived
# with the exact size. test-persist.sh only covers a ~10-byte file.
# Usage: bash scripts/build.sh && bash scripts/test-persist-big.sh
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin
ISO=build/ChocoOS.iso
VOL=build/chocovol-big.img
LOG1=build/serial-big1.log
LOG2=build/serial-big2.log
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$VOL" "$LOG1" "$LOG2"
dd if=/dev/zero of="$VOL" bs=512 count=65536 status=none
mformat -i "$VOL" -F -v CHOCOVOL :: >/dev/null 2>&1
sk() { case "$1" in
    [a-z0-9]) echo "$1" ;; ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')" ;; esac; }
run_boot() {
  local log="$1"
  coproc Q { exec qemu-system-x86_64 -m 256M -display none -accel tcg,tb-size=64 \
    -serial file:"$log" -boot order=d -no-reboot \
    -drive file="$VOL",format=raw,if=none,id=hdrvol \
    -device ide-hd,bus=ide.0,unit=0,drive=hdrvol -cdrom "$ISO" -monitor stdio >/dev/null 2>&1; }
  local ok=0
  for i in $(seq 1 90); do sleep 1; grep -q "choco> " "$log" 2>/dev/null && { ok=1; break; }; done
  [ "$ok" -eq 1 ] || { echo "FAIL: no prompt ($log)"; return 1; }
}
kill_boot() { printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true; sleep 1
  taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true; }
type_line() { local s="$1" k
  for ((k=0;k<${#s};k++)); do printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"; sleep 0.12; done
  printf 'sendkey ret\n' >&"${Q[1]}"; sleep 1.3; }

echo "[big] boot1: cp bin/fileman (77568 B) -> disk/f.bin"
run_boot "$LOG1" || exit 1
type_line "cp bin/fileman disk/f.bin"
type_line "ls"
kill_boot
echo "[big] boot2: verify"
run_boot "$LOG2" || exit 1
type_line "ls"
kill_boot

pass=0; fail=0
check() { if grep -qF "$1" "$2" 2>/dev/null; then echo "PASS: $3"; pass=$((pass+1));
          else echo "FAIL: $3 (missing '$1' in $2)"; fail=$((fail+1)); fi; }
check "77568 bytes"       "$LOG1" "boot1: cp reported 77568 bytes"
check "disk/f.bin (77568 B)" "$LOG2" "boot2: large file persisted with correct size"
echo "tmp-big: $pass passed, $fail failed"
rm -f "$VOL"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# Headless E2E test for the persistent FAT32 ATA volume (chocovol.img).
# Boot 1: create a file under /disk, write data, quit.
# Boot 2 (same image): read the file back — proves the write hit the volume.
# Usage: bash scripts/test-persist.sh
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
VOL=build/chocovol-test.img
LOG1=build/serial-p1.log
LOG2=build/serial-p2.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$VOL" "$LOG1" "$LOG2"

# Fresh FAT32 volume (mtools).
dd if=/dev/zero of="$VOL" bs=512 count=65536 status=none
mformat -i "$VOL" -F -v CHOCOVOL :: >/dev/null 2>&1
mmd -i "$VOL" ::docs >/dev/null 2>&1 || true

sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')" ;;
  esac
}

run_boot() {
  local log="$1"
  # Volume on primary master (typical HD spot); CD-ROM takes secondary
  # master via the default -cdrom wiring. The kernel ATA scan finds either.
  coproc Q { exec qemu-system-x86_64 -m 256M -display none \
    -accel tcg,tb-size=64 -serial file:"$log" -boot order=d -no-reboot \
    -drive file="$VOL",format=raw,if=none,id=hdrvol \
    -device ide-hd,bus=ide.0,unit=0,drive=hdrvol \
    -cdrom "$ISO" \
    -monitor stdio >/dev/null 2>&1; }
  # Pin the PID right after spawn so a bail-out always kills the right VM.
  QPID="${Q_PID:-$!}"
  local ok=0
  for i in $(seq 1 90); do
    sleep 1
    if grep -q "choco> " "$log" 2>/dev/null; then ok=1; break; fi
  done
  if [ "$ok" -ne 1 ]; then
    echo "FAIL: shell prompt never appeared ($log)"
    kill "$QPID" 2>/dev/null || true
    taskkill //F //PID "${Q_PID:-0}" >/dev/null 2>&1 || true
    return 1
  fi
  return 0
}

kill_boot() {
  printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
  sleep 1
  if [ -n "${Q_PID:-}" ]; then
    kill "${Q_PID}" 2>/dev/null || taskkill //F //PID "${Q_PID}" >/dev/null 2>&1 || true
  fi
}

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 1.2
}

echo "[persist] boot 1: create + write on the volume..."
run_boot "$LOG1" || exit 1
# mkfile drives the writable layer (no shell redirection needed for unit tests).
type_line "mkfile disk/notes2.txt persist-me"
type_line "cat disk/notes2.txt"
type_line "ls"
kill_boot

# Boot 2: the same image must be seen again (DETECTION), and any file we
# managed to write must still be there.
echo "[persist] boot 2: volume detection + contents..."
run_boot "$LOG2" || exit 1
type_line "cat disk/notes2.txt"
kill_boot

pass=0; fail=0
check() {
  if grep -qF "$1" "$2" 2>/dev/null; then
    echo "PASS: $3"; pass=$((pass + 1))
  else
    echo "FAIL: $3 (missing '$1' in $2)"; fail=$((fail + 1))
  fi
}

check "writable volume" "$LOG1" "boot 1: ATA volume mounted writable"
check "persist-me"     "$LOG1" "boot 1: cat right after write"
check "writable volume" "$LOG2" "boot 2: same volume detected again"
check "persist-me"     "$LOG2" "boot 2: file survived the reboot"
check "disk/docs/"      "$LOG1" "docs/ from mtools present"

# Structural validation of the volume itself (fsck on the host).
if [ "$fail" -eq 0 ] && bash scripts/check-fat.sh "$VOL" >/dev/null 2>&1; then
  echo "PASS: volume passes host fsck.fat"
  pass=$((pass + 1))
elif [ "$fail" -ne 0 ]; then
  echo "SKIP: fsck check (earlier failures)"
fi

echo "test-persist: $pass passed, $fail failed"
rm -f "$VOL"
[ "$fail" -eq 0 ]

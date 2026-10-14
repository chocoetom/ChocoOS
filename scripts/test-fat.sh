#!/usr/bin/env bash
# Headless E2E test for the FAT32 ramdisk (/disk).
# Boots the ISO, types a few shell commands through the QEMU monitor (PS/2
# keyboard injection) and asserts the expected output on the serial log.
# Usage: bash scripts/make-fatdisk.sh && bash scripts/build.sh   # or build-local
#        bash scripts/test-fat.sh
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"

ISO=build/ChocoOS.iso
LOG=build/serial-fat.$$.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

# The log name is unique per run, so no stale emulator can feed it old output
# and there is nothing to kill: other sessions may be booting this tree too.
fresh "$LOG"

# QEMU sendkey name for one character.
sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')" ;;
  esac
}

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -monitor stdio >/dev/null 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
if ! grep -q "choco> " "$LOG" 2>/dev/null; then
  [ -s "$LOG" ] || echo "       ($LOG is empty: the emulator never wrote to it)"
  echo "FAIL: shell prompt never appeared"; kill "${Q_PID:-0}" 2>/dev/null || true; exit 1
fi

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 1.2
}

echo "[test-fat] driving the shell..."
type_line "cat disk/README.TXT"
type_line "cat disk/docs/bigfile.txt"
type_line "cat disk/Documents/notes.txt"
type_line "ls"
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true

# The shell echoes what it types, so a missing echo means the monitor pipe was
# broken - without this the content checks below would fail as if FAT were.
if ! grep -aq "choco> cat disk/README.TXT" "$LOG" 2>/dev/null; then
  echo "FAIL: keystrokes never reached the guest (sendkey pipeline broken)"
  kill "${Q_PID:-0}" 2>/dev/null || true
  exit 1
fi

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "fat32] mounted" "FAT32 ramdisk mounted at boot"
check "ChocoOS FAT32 ramdisk" "read disk/README.TXT (root, single cluster)"
check "line 001: the quick brown fox" "read bigfile.txt first cluster"
check "line 080: the quick brown fox" "read bigfile.txt last cluster (chain)"
check "Documents/notes.txt - a plain text file" "read nested Documents/notes.txt"
check "disk/hello world.txt" "ls lists LFN 'hello world.txt'"
check "disk/Documents/Todo List 2026.txt" "ls lists LFN in a subdirectory"
check "disk/docs/bigfile.txt" "ls lists a /disk file"

echo "test-fat: $pass passed, $fail failed"
# Unique log names pile up otherwise; keep them only when something failed.
[ "$fail" -eq 0 ] && rm -f "$LOG"
[ "$fail" -eq 0 ]

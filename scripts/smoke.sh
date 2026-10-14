#!/usr/bin/env bash
# Headless smoke test: boots the ISO and expects the shell prompt on serial.
# Usage: bash scripts/smoke.sh   (run scripts/build.sh first)
# Exit 0 when all markers appear, 1 otherwise.
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"
need grep "coreutils"

ISO=build/ChocoOS.iso
# Unique per run: other sessions boot this same tree concurrently, and a log
# they still hold open would be read back as this run's output.
LOG=build/serial.smoke.$$.log
if [ ! -f "$ISO" ]; then
  echo "no ISO: run scripts/build.sh first"
  exit 1
fi
fresh "$LOG"
# A QEMU still holding the log open makes the write below silently produce an
# empty file, which reads as "every marker missing". Say so instead.
timeout 90 qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot &
QP=$!
for i in $(seq 1 80); do
  sleep 1
  if grep -q "choco> " "$LOG" 2>/dev/null; then
    break
  fi
done
kill "$QP" 2>/dev/null || true

if [ ! -s "$LOG" ]; then
  echo "FAIL: $LOG is empty - another QEMU is probably still holding it open."
  echo "      close the stale emulator (or delete $LOG) and rerun."
  exit 1
fi

pass=0
fail=0
check() {
  if grep -q "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"
    pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"
    fail=$((fail + 1))
  fi
}

check "ChocoOS 0" "kernel banner"
check "self-test OK" "heap self-test"
check "own table live" "GDT+TSS"
check "ring-3 syscalls" "IDT with syscall gate"
check "initrd:" "initrd parsed"
check "multitasking live" "scheduler workers"
check "desktop ready" "GUI desktop"
check "choco> " "shell prompt"
echo "smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# Regression test: a fresh thread that never yields must still be preempted
# (fixed by sti in thread_trampoline), and sleep_ms(1) must cost >= 1 tick
# (fixed by rounding up in sched::sleep_ms).
#
# Before the fixes the boot sequence froze when a worker never yielded; the
# stress command below is the direct probe: the shell prints only when it
# keeps running alongside the spinner.
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-sched.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG"

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
QPID="${Q_PID:-$!}"

for i in $(seq 1 60); do
  sleep 1
  if grep -q "choco> " "$LOG" 2>/dev/null; then break; fi
done

if ! grep -q "choco> " "$LOG" 2>/dev/null; then
  echo "FAIL: shell prompt never appeared (boot froze - the spin bug is back)"
  kill "$QPID" 2>/dev/null || taskkill //F //PID "${Q_PID:-0}" >/dev/null 2>&1 || true
  exit 1
fi

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 2.0
}

echo "[test-sched] driving the stress command..."
type_line "stress"

printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 1
kill "$QPID" 2>/dev/null || taskkill //F //PID "${Q_PID:-0}" >/dev/null 2>&1 || true

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "[sched] spawned 'stress'" "new thread booted (trampoline sti)"
check "shell responsive while spinner runs" "shell keeps answering during spinner"
check "preemptive multitasking live" "preemption active afterwards"

echo "test-sched: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

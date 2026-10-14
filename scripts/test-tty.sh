#!/usr/bin/env bash
# Headless E2E test for the console tty line discipline (FEATURE-TTY-FULL).
#
# Boots the ISO, drives /bin/ttytest through the QEMU monitor (PS/2 keyboard
# injection) and asserts on the serial log: canonical input is echoed and
# delivered as a finished line, TCSETS switches to raw mode, arrow keys arrive
# as ANSI escape sequences, and Ctrl+C (ISIG) terminates the reader.
# Usage: bash scripts/build.sh && bash scripts/test-tty.sh
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/c/msys64/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-tty.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

# Kill any stale emulator holding the log file (Windows releases late).
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG"

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
  echo "FAIL: shell prompt never appeared"; kill "${Q_PID:-0}" 2>/dev/null || true; exit 1
fi

send() { printf '%s\n' "$1" >&"${Q[1]}"; }
type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    send "sendkey $(sk "${s:$k:1}")"
    sleep 0.12
  done
  send "sendkey ret"
  sleep 1.4
}

echo "[test-tty] phase A: canonical line + raw arrows..."
type_line "run bin/ttytest"
sleep 0.8
type_line "ok"                 # canonical read -> "ok\n" (n=3, echoed)
sleep 0.6
send "sendkey up";   sleep 0.15
send "sendkey down"; sleep 0.6

echo "[test-tty] phase B: Ctrl+C kills the blocked reader..."
sleep 1.5
type_line "run bin/ttytest"
sleep 1.0
send "sendkey ctrl-c"
sleep 1.5

printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "TTYTEST start"        "program starts"
check "CANON n=3 [ok<NL>]"   "canonical line delivered with echo"
check "TTYTEST raw"          "TCSETS switched the tty to raw mode"
check "K=27"                 "arrow delivered ESC in raw mode"
check "K=91"                 "arrow delivered CSI '[' in raw mode"
check "K=65"                 "UP arrow -> 'A' (ESC [ A)"
check "K=66"                 "DOWN arrow -> 'B' (ESC [ B)"
check "TTYTEST done"         "program finished normally"
check "killed"               "Ctrl+C (ISIG) terminated the reader"

echo "test-tty: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

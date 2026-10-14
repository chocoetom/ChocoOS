#!/usr/bin/env bash
# Headless E2E test for the shell pipe operator '|' (Tier 3).
# Pipeline is sequential (producer's output is buffered to a scratch file, then
# fed to the consumer's stdin), so a finite producer is required.
# Usage: bash scripts/build-local.sh && bash scripts/test-shell3.sh
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/c/msys64/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-shell3.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG"

sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;;
    '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    '>' ) echo shift-dot ;; '<' ) echo shift-comma ;; '&' ) echo shift-7 ;;
    '|' ) echo shift-backslash ;; ';' ) echo semicolon ;; '=' ) echo equal ;;
    '$' ) echo shift-4 ;; '?' ) echo shift-slash ;;
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
    sleep 0.1
  done
  send "sendkey ret"
  sleep 1.3
}

echo "[test-shell3] exercising pipes..."
type_line "echo PIPEME | run bin/ttytest"   # builtin -> program stdin
type_line "echo AB | run bin/ttytest"       # 2-byte line
type_line "ls"                              # scratch files must be cleaned up
type_line "run bin/lhello &"                # '&': background job, prompt returns
type_line "echo AFTERBG"                    # shell must stay responsive
type_line "poweroff"

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}
check_absent() {
  if grep -qE "$1" "$LOG" 2>/dev/null; then
    echo "FAIL: $2 (unexpected match '$1')"; fail=$((fail + 1))
  else
    echo "PASS: $2"; pass=$((pass + 1))
  fi
}

check "CANON n=7 [PIPEME<NL>]"  "pipe: builtin output reached the program stdin"
check "CANON n=3 [AB<NL>]"      "pipe: 2-byte line delivered"
check_absent "^  \.p[0-9]"      "scratch pipe files are removed afterwards"
check "[bg] bin/lhello pid"     "background job '&' started, prompt returned"
check "[proc] job pid="         "background job reaped by the ambient loop"
check "AFTERBG"                 "shell stayed responsive after '&'"
check "powering off"            "poweroff still works"

echo "test-shell3: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

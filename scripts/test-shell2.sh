#!/usr/bin/env bash
# Headless E2E test for the shell pipeline (Tier 2): redirection, sequencing,
# short-circuit (&& / ||) and $? expansion.
# Assertions use derived strings (file sizes, expanded status) so the shell's
# own character echo of the command line cannot satisfy them.
# Usage: bash scripts/build-local.sh && bash scripts/test-shell2.sh
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/c/msys64/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-shell2.log
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
  sleep 1.2
}

echo "[test-shell2] exercising Tier 2 (redirection + chaining)..."
type_line "echo 12345 > n.txt"          # truncate: 6 bytes
type_line "ls"
type_line "echo 67890 >> n.txt"         # append: 12 bytes
type_line "ls"
type_line "nosuchcmd ; echo S=\$?"
type_line "echo ok && echo P=\$?"
type_line "nosuchcmd && echo A ; echo N=\$?"
type_line "nosuchcmd || echo FALLBACK=\$?"
type_line "echo ok > in.txt"
type_line "run bin/ttytest < in.txt"    # stdin redirect
type_line "run bin/umon > m.txt"        # external stdout redirect
type_line "cat m.txt"
type_line "poweroff"

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "n.txt (6 B)"          "'>' truncates the target (6 bytes)"
check "n.txt (12 B)"         "'>>' appends (12 bytes)"
check "S=127"                "\$? reports unknown command status"
check "P=0"                  "'&&' runs the next command on success"
check "N=127"                "'&&' short-circuits (status preserved)"
check "FALLBACK=127"         "'||' runs on failure"
check "CANON n=3 [ok<NL>]"   "'<' feeds stdin from a file"
check "TTYTEST eof"          "stdin file is consumed, then EOF"
check "MemTotal"             "external program stdout redirected to a file"
check "powering off"         "poweroff still works"

echo "test-shell2: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

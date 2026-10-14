#!/usr/bin/env bash
# Headless E2E test for the shell filesystem commands (Tier 1).
# Boots the ISO, drives the shell through the QEMU monitor and asserts the
# mkdir/touch/cp/mv/rm/cd/pwd/tree/df/uname/create/delete flow plus a poweroff.
# Usage: bash scripts/build-local.sh && bash scripts/test-shell.sh
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/c/msys64/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-shell.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG"

sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    '_' ) echo shift-minus ;;
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
  sleep 1.1
}

echo "[test-shell] exercising Tier 1 fs commands..."
type_line "mkdir tdir"
type_line "touch tdir/a.txt"
type_line "cp motd.txt tdir/b.txt"
type_line "ls tdir"
type_line "cd tdir"
type_line "pwd"
type_line "mv b.txt c.txt"
type_line "tree"
type_line "cd .."
type_line "pwd"
type_line "rm tdir/c.txt"
type_line "touch tdir/a.txt"     # exists -> no-op path
type_line "df"
type_line "uname"
type_line "ls"                   # legacy full listing regression
type_line "poweroff"

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "mkdir: tdir"                   "mkdir creates a directory"
check "touch: tdir/a.txt"             "touch creates a file"
check "cp: motd.txt -> tdir/b.txt"    "cp copies a file"
check "mv: tdir/b.txt -> tdir/c.txt"  "mv renames a file"
check "rm: tdir/c.txt"                "rm unlinks a file"
check "/tdir"                         "pwd reflects cd"
check "c.txt"                         "tree lists the file"
check "initrd"                        "df reports the initrd"
check "ChocoOS 0.7 x86_64"            "uname identifies the system"
check "motd.txt"                      "ls (no arg) still lists the whole tree"
check "powering off"                  "poweroff shuts down"

echo "test-shell: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

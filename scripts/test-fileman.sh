#!/usr/bin/env bash
# Headless GUI smoke for the File Explorer.
# Boots the ISO, launches bin/fileman from the shell (QEMU monitor keystrokes),
# then asserts on a framebuffer screendump that the window drew its Fluent
# palette (folder icons + the selected-row fill).
# Usage: bash scripts/test-fileman.sh
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"
need python3 "python"

ISO=build/ChocoOS.iso
RUN=$$
LOG=build/serial-fileman.$RUN.log
ROOT=build/fm_root.$RUN.ppm
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

fresh "$LOG"
fresh "$ROOT"

sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;;
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
  echo "FAIL: shell prompt never appeared"; exit 1
fi

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.28
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 1.5
}
shot() { printf 'screendump %s\n' "$1" >&"${Q[1]}"; sleep 2; }

echo "[test-fileman] launching the explorer..."
type_line "run bin/fileman"
if ! grep -aq "choco> run bin/fileman" "$LOG" 2>/dev/null; then
  echo "FAIL: keystrokes never reached the guest (sendkey pipeline broken)"
  printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
  exit 1
fi
sleep 4
shot "$ROOT"                 # root listing
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true

pass=0; fail=0
check() { if [ "$1" = "1" ]; then echo "PASS: $2"; pass=$((pass + 1)); else echo "FAIL: $2"; fail=$((fail + 1)); fi; }

count_color() {
  if command -v python3 >/dev/null 2>&1; then PY=python3
  elif command -v python >/dev/null 2>&1; then PY=python
  else PY=/c/Python314/python.exe; fi
  "$PY" scripts/ppm_count.py "$1" "$2"
}

if [ ! -f "$ROOT" ]; then
  echo "FAIL: missing screendump (did the window open?)"; exit 1
fi

# The explorer's palette is unmistakable: folder-icon orange (UI_FOLDER) and
# the selected-row fill (UI_SEL) only come from fileman's list.
orange=$(count_color "$ROOT" F5A524)
sel=$(count_color "$ROOT" 2D2A40)
check "$([ "${orange:-0}" -gt 0 ] && echo 1 || echo 0)" \
      "fileman window drew (folder icons: ${orange} px)"
check "$([ "${sel:-0}" -gt 0 ] && echo 1 || echo 0)" \
      "listing rendered with a selection (purple: ${sel} px)"

echo "test-fileman: $pass passed, $fail failed"
# Keep the frame only when something failed; the run id makes names unique.
[ "$fail" -eq 0 ] && rm -f "$LOG" "$ROOT"
[ "$fail" -eq 0 ]

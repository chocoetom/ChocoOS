#!/usr/bin/env bash
# Integrated test: HTTP fetch (TCP) + persistent write (FAT32/ATA).
# Proves both subsystems work together under memory pressure.
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
VOL=build/chocovol-test.img
LOG1=build/serial-http1.log
LOG2=build/serial-http2.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$VOL" "$LOG1" "$LOG2"

# Fresh FAT32 volume (mtools).
dd if=/dev/zero of="$VOL" bs=512 count=65536 status=none
mformat -i "$VOL" -F -v CHOCOVOL :: >/dev/null 2>&1

sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    ':' ) echo colon ;;
    '=' ) echo equal ;;
    [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')" ;;
  esac
}

run_boot() {
  local log="$1"
  coproc Q { exec qemu-system-x86_64 -m 256M -display none \
    -accel tcg,tb-size=64 -serial file:"$log" -boot order=d -no-reboot \
    -device ide-hd,bus=ide.0,unit=0,drive=hdrvol \
    -drive file="$VOL",format=raw,if=none,id=hdrvol \
    -cdrom "$ISO" \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -monitor stdio >/dev/null 2>&1; }
  QPID="${Q_PID:-$!}"
  local ok=0
  for i in $(seq 1 90); do
    sleep 1
    if grep -q "choco> " "$log" 2>/dev/null; then ok=1; break; fi
  done
  if [ "$ok" -ne 1 ]; then
    echo "FAIL: shell prompt never appeared ($log)"
    kill "$QPID" 2>/dev/null || taskkill //F //PID "${Q_PID:-0}" >/dev/null 2>&1 || true
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

prompt_count() { grep -c "choco> " "$1" 2>/dev/null || echo 0; }

# Type a command and wait for the NEXT shell prompt in the serial log.
# Fixed sleeps race with slow commands (a 200 KiB download + ~400 sector PIO
# writes takes a few seconds) and the old script would kill QEMU mid-flush,
# making the persistence checks fail intermittently.
type_line() {
  local s="$1" k log="$2" before after
  before=$(prompt_count "$log")
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.06
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  for i in $(seq 1 200); do
    sleep 0.5
    after=$(prompt_count "$log")
    if [ "$after" -gt "$before" ]; then return 0; fi
  done
  echo "WARN: no prompt after '$s' in $log"
}

echo "[net+disk] boot 1: fetch via TCP, save to persistent volume..."
run_boot "$LOG1" || exit 1
type_line "httpget 10.0.2.2 18080 /bigfile.bin disk/fetched.bin" "$LOG1"
type_line "ls" "$LOG1"
kill_boot

echo "[net+disk] boot 2: verify downloaded file survived reboot..."
run_boot "$LOG2" || exit 1
type_line "ls" "$LOG2"
type_line "cat disk/fetched.bin" "$LOG2"
kill_boot

pass=0; fail=0
check() {
  if grep -qF "$1" "$2" 2>/dev/null; then
    echo "PASS: $3"; pass=$((pass + 1))
  else
    echo "FAIL: $3 (missing '$1' in $2)"; fail=$((fail + 1))
  fi
}

check "writable volume"      "$LOG1" "boot 1: FAT32 volume mounted"
check "saved 211148"           "$LOG1" "boot 1: httpget wrote to disk"
check "disk/fetched.bin"     "$LOG1" "boot 1: file listed after download"
check "writable volume"      "$LOG2" "boot 2: same volume detected again"
check "disk/fetched.bin"     "$LOG2" "boot 2: HTTP result persisted"
check "line 0000"            "$LOG2" "boot 2: start of file readable"

echo "test-netdisk: $pass passed, $fail failed"
if [ -z "${KEEP_VOL:-}" ]; then rm -f "$VOL"; else echo "[net+disk] volume kept at $VOL"; fi
[ "$fail" -eq 0 ]

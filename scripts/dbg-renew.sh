#!/usr/bin/env bash
# Narrower repro: boot WITH link up (dhcp binds), THEN kill the link and
# renew. Distinguishes "dhcp+dead backend freezes" from boot-timing issues.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-renew.log
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
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -qmp tcp:127.0.0.1:4444,server=on,wait=off \
  -monitor stdio >/dev/null 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
sleep 12  # let dhcp bind on the live net
python3 scripts/qmp_link.py down
sleep 1
type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 2.5
}
type_line "dhcp renew"
sleep 16  # 4x2s discovers + margin, all skipped-fast (no kicks)
type_line "dhcp"
type_line "ifconfig"
sleep 2
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
echo "=== dhcp lines ==="
grep -a "dhcp" "$LOG" | tail -12

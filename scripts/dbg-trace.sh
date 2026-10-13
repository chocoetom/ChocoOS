#!/usr/bin/env bash
# Debug helper: boot with QEMU virtio tracing to see if notifies and
# queue processing reach the device. Types one ping, saves trace.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-trace.log
rm -f "$LOG" build/trace.log

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
  -object filter-dump,id=dump0,netdev=n0,file=build/dbg-trace.pcap \
  -trace "virtio*" -trace "virtqueue*" -trace "vring*" -trace "virtio_queue_notify" -trace "virtqueue_pop" -trace "virtio_notify" -D build/trace.log \
  -monitor stdio >build/mon-trace.log 2>build/qemu-stderr.log; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 6
}

type_line "netdump"
type_line "netpoke 0 1"
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 2
echo "trace lines: $(wc -c < build/trace.log)"
grep -a -o -e "virtio_[a-z_0-9]*" -e "virtqueue_[a-z_0-9]*" -e "vring_[a-z_0-9]*" build/trace.log 2>/dev/null | sort | uniq -c | sort -rn | head -30
echo "=== qemu stderr ==="
cat build/qemu-stderr.log

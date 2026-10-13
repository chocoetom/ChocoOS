#!/usr/bin/env bash
# Debug helper: boot with the same NIC args as net-test.sh, ask the QEMU
# monitor for `info pci` / `info network`, save monitor output to mon.log.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-dbg.log
MON=build/mon.log
rm -f "$LOG" "$MON"

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -object filter-dump,id=dump0,netdev=n0,file=build/dbg.pcap \
  -monitor stdio >"$MON" 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
printf 'info pci\n' >&"${Q[1]}"
sleep 2
printf 'info virtio\n' >&"${Q[1]}"
sleep 2
printf 'info virtio /machine/peripheral-anon/device[0]/virtio-backend\n' >&"${Q[1]}"
sleep 2
printf 'help info virtio\n' >&"${Q[1]}"
sleep 1
printf 'info mtree\n' >&"${Q[1]}"
sleep 3
printf 'info network\n' >&"${Q[1]}"
sleep 2
printf 'info status\n' >&"${Q[1]}"
sleep 1
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 2
echo "=== mon.log ==="
cat "$MON"

#!/usr/bin/env bash
# Debug helper: boot with QMP enabled, dump x-query-virtio-status after
# the guest driver has initialized (proves backend-side queue state).
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-qmp.log
rm -f "$LOG"

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -object filter-dump,id=dump0,netdev=n0,file=build/dbg-qmp.pcap \
  -qmp tcp:127.0.0.1:4444,server=on,wait=off \
  -monitor none >build/qemu-qmp-stderr.log 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
if ! grep -q "choco> " "$LOG" 2>/dev/null; then
  echo "FAIL: no prompt"
fi
sleep 3
python3 scripts/dbg-qmp.py
echo "--- qemu stderr tail ---"
tail -5 build/qemu-qmp-stderr.log 2>/dev/null || true
if [ -n "${Q_PID:-}" ]; then kill "$Q_PID" 2>/dev/null || true; fi
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true

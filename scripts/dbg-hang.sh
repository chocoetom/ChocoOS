#!/usr/bin/env bash
# Repro the link-down freeze, then probe QEMU liveness (QMP) + guest state.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-hang.log
rm -f "$LOG"

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -qmp tcp:127.0.0.1:4444,server=on,wait=off \
  -monitor stdio >build/mon-hang.log 2>build/qemu-hang-stderr.log; }

sleep 2
if [ "${LINK_DOWN:-1}" -eq 1 ]; then
  python3 scripts/qmp_link.py down
fi
echo "--- waiting for boot + first discover ---"
for i in $(seq 1 75); do
  sleep 1
  grep -q "discover try 1" "$LOG" 2>/dev/null && break
done
sleep 10
echo "--- serial tail ---"
tail -3 "$LOG"
echo "--- qmp probe (is QEMU alive?) ---"
timeout 20 python3 scripts/qmp_stat.py || echo "QMP DEAD"
echo "--- registers x12 (0.5s apart) ---"
for k in 1 2 3 4 5 6 7 8 9 10 11 12; do
  printf 'info registers\n' >&"${Q[1]}"
  sleep 0.5
done
printf 'info pic\n' >&"${Q[1]}"
sleep 1
echo "--- serial tail ---"
tail -4 "$LOG"
echo "--- qemu cpu ---"
powershell.exe -NoProfile -Command "Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Select-Object CPU, Id" 2>&1 | head -5
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 1
if [ -n "${Q_PID:-}" ]; then kill "$Q_PID" 2>/dev/null || true; fi
echo done

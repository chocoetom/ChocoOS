#!/usr/bin/env bash
# E2E for the TCP server side (Fase 18 criterion: client and server exchange
# data in QEMU). Boots with hostfwd tcp::18081->guest:18081, runs /bin/lechoserv
# (bind/listen/accept/read/write) and connects from the HOST with Python.
# Usage: bash scripts/build.sh && bash scripts/test-sock-serv.sh
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"
need grep "coreutils"

ISO=build/ChocoOS.iso
LOG="build/serial.sock-serv.$$.log"
fresh "$LOG"
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0,hostfwd=tcp::18081-:18081 \
  -device virtio-net-pci,netdev=n0,ioeventfd=off -monitor stdio >/dev/null 2>&1; }
QPID="${Q_PID:-$!}"

for i in $(seq 1 90); do sleep 1; grep -q "choco> " "$LOG" 2>/dev/null && break; done
if ! grep -q "choco> " "$LOG" 2>/dev/null; then echo "FAIL: no shell prompt"; exit 1; fi

for c in r u n spc b i n slash l e c h o s e r v; do
  printf 'sendkey %s\n' "$c" >&"${Q[1]}"; sleep 0.1
done
printf 'sendkey ret\n' >&"${Q[1]}"
sleep 3   # server binds, listens and blocks in accept()

reply=$(python3 -c "
import socket
s=socket.create_connection(('127.0.0.1',18081),timeout=10)
s.sendall(b'PING\n')
d=s.recv(16)
s.close()
print(d.decode(errors='replace').strip())
" 2>/dev/null)

printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 1
kill "$QPID" 2>/dev/null || true
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true

pass=0; fail=0
check() { if grep -qF -- "$1" "$2" 2>/dev/null; then echo "PASS: $3"; pass=$((pass+1)); else echo "FAIL: $3 (missing '$1')"; fail=$((fail+1)); fi; }
check "SRV listen" "$LOG" "server bound + listened"
check "SRV accept" "$LOG" "server accepted the host connection"
check "SRV done"   "$LOG" "server served the request and finished"
if echo "$reply" | grep -qF "PONG"; then
  echo "PASS: host client got the reply (data exchanged)"; pass=$((pass + 1))
else
  echo "FAIL: host client reply was '$reply'"; fail=$((fail + 1))
fi

echo "test-sock-serv: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

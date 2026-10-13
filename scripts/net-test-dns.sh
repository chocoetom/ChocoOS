#!/usr/bin/env bash
# Passo 3 (net-3): DNS via UDP (o QEMU encaminha 10.0.2.3 para o DNS do host).
# Resolve example.com (A record estavel) e um .invalid (NXDOMAIN ou timeout).
# Confere serial + pcap (query/response via scripts/pcap.py).
# Uso: bash scripts/build-local.sh && bash scripts/net-test-dns.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-dns.log
PCAP=build/dns.pcap
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG" "$PCAP"

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
  -object filter-dump,id=dump0,netdev=n0,file="$PCAP" \
  -monitor stdio >/dev/null 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
if ! grep -q "choco> " "$LOG" 2>/dev/null; then
  echo "FAIL: shell prompt never appeared"
  if [ -n "${Q_PID:-}" ]; then kill "$Q_PID" 2>/dev/null || true; fi
  exit 1
fi

sleep 12  # let dhcp bind first (dns server comes from it)

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 7
}

echo "[net-test-dns] driving the shell..."
type_line "dns example.com"
type_line "dns nonexistent-domain-12345.invalid"
sleep 2
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

# example.com tem A record estavel (93.184.216.34 ha decadas).
check "example.com -> " "dns answered example.com with an address"
if grep -qF "example.com -> 93.184.216.34" "$LOG" 2>/dev/null; then
  echo "PASS: example.com resolved to the expected A record"; pass=$((pass + 1))
else
  echo "INFO: example.com resolved to a different A record (anycast?) — see log"
fi
# .invalid nunca resolve: NXDOMAIN ou timeout, ambos aceitos.
if grep -qF "NXDOMAIN" "$LOG" 2>/dev/null; then
  echo "PASS: .invalid -> NXDOMAIN"; pass=$((pass + 1))
elif grep -qF "nonexistent-domain-12345.invalid: timeout" "$LOG" 2>/dev/null; then
  echo "PASS: .invalid -> timeout (no upstream DNS here)"; pass=$((pass + 1))
else
  echo "FAIL: .invalid produced neither NXDOMAIN nor timeout"; fail=$((fail + 1))
fi

echo "--- pcap ---"
if [ -f "$PCAP" ]; then
  python3 scripts/pcap.py "$PCAP"
  if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "dns query.*example.com"; then
    echo "PASS: pcap has DNS query for example.com"; pass=$((pass + 1))
  else
    echo "FAIL: pcap missing DNS query"; fail=$((fail + 1))
  fi
  if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "dns resp.*rcode=0"; then
    echo "PASS: pcap has DNS response rcode=0"; pass=$((pass + 1))
  else
    echo "FAIL: pcap missing DNS response"; fail=$((fail + 1))
  fi
  echo "inspect: python3 scripts/pcap.py $PCAP"
else
  echo "FAIL: no pcap generated"; fail=$((fail + 1))
fi

echo "net-test-dns: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# Passo 2 (net-2): UDP + DHCP, teste anti-fallback.
# Sobe o QEMU numa rede DIFERENTE do default (10.0.5.0/24, dhcpstart .20):
# se o cliente funcionar, o IP final eh o concedido (10.0.5.20), nao o
# fallback compilado (10.0.2.15). Confere serial + pcap (discover/offer/
# request/ack via scripts/pcap.py).
# Uso: bash scripts/build-local.sh && bash scripts/net-test-dhcp.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-dhcp.log
PCAP=build/dhcp.pcap
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
  -netdev user,id=n0,net=10.0.5.0/24,dhcpstart=10.0.5.20 \
  -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -object filter-dump,id=dump0,netdev=n0,file="$PCAP" \
  -monitor stdio >/dev/null 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
if ! grep -q "choco> " "$LOG" 2>/dev/null; then
  echo "FAIL: shell prompt never appeared"; kill "${Q_PID:-0}" 2>/dev/null || true; exit 1
fi

# DHCP runs in background (~2-6s); give it room before asking.
sleep 12

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 2.5
}

echo "[net-test-dhcp] driving the shell..."
type_line "dhcp"
type_line "ifconfig"
type_line "udpstat"
type_line "ping 10.0.5.2 2"
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
check_absent() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "FAIL: $2 (forbidden '$1' present)"; fail=$((fail + 1))
  else
    echo "PASS: $2"; pass=$((pass + 1))
  fi
}

check "[dhcp] bound 10.0.5.20" "DHCP bound the granted address"
check "src dhcp" "ifconfig shows dhcp source"
check "ip 10.0.5.20" "ifconfig shows granted IP"
check_absent "fallback" "no fallback was used"
check "[ping] 2/2 replies" "ping to 10.0.5.2 via DHCP-configured gateway"

echo "--- pcap ---"
if [ -f "$PCAP" ]; then
  python3 scripts/pcap.py "$PCAP"
  for pat in "dhcp discover" "dhcp offer" "dhcp request" "dhcp ack"; do
    if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "$pat"; then
      echo "PASS: pcap has $pat"; pass=$((pass + 1))
    else
      echo "FAIL: pcap missing $pat"; fail=$((fail + 1))
    fi
  done
  echo "inspect: python3 scripts/pcap.py $PCAP"
else
  echo "FAIL: no pcap generated"; fail=$((fail + 1))
fi

echo "net-test-dhcp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

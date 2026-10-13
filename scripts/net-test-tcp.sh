#!/usr/bin/env bash
# Passo 4 (net-4): TCP cliente + HTTP/1.0 GET.
# Sobe um servidor HTTP no host (127.0.0.1:18080, fixture scripts/tcp-root)
# e o guest busca /tcp-hello.txt via 10.0.2.2 (o gateway encaminha p/ host).
# Confere serial (status 200 + marcador) + pcap (SYN/SYN-ACK/GET/200/FIN).
# Uso: bash scripts/build-local.sh && bash scripts/net-test-tcp.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-tcp.log
PCAP=build/tcp.pcap
PORT=18080
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG" "$PCAP"

# Fixture HTTP no host.
python3 -m http.server "$PORT" --directory scripts/tcp-root --bind 127.0.0.1 \
  >build/http-server.log 2>&1 &
SRV=$!
sleep 2

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
  kill "$SRV" 2>/dev/null || true
  exit 1
fi

sleep 12  # dhcp first

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 10
}

echo "[net-test-tcp] driving the shell..."
type_line "tcpstat"
type_line "httpget 10.0.2.2 18080 /tcp-hello.txt"
type_line "tcpstat"
sleep 2
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 1
kill "$SRV" 2>/dev/null || true

pass=0; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "HTTP/1.0 200" "server returned 200 OK"
check "CHOCO_TCP_OK" "fixture body arrived intact"
check "[http] done:" "httpget completed"

echo "--- pcap ---"
if [ -f "$PCAP" ]; then
  python3 scripts/pcap.py "$PCAP"
  if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "flags=SA"; then
    echo "PASS: pcap has SYN-ACK handshake"; pass=$((pass + 1))
  else
    echo "FAIL: pcap missing SYN-ACK"; fail=$((fail + 1))
  fi
  if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "GET /tcp-hello.txt"; then
    echo "PASS: pcap has HTTP GET"; pass=$((pass + 1))
  else
    echo "FAIL: pcap missing HTTP GET"; fail=$((fail + 1))
  fi
  if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "200 OK"; then
    echo "PASS: pcap has 200 OK"; pass=$((pass + 1))
  else
    echo "FAIL: pcap missing 200 OK"; fail=$((fail + 1))
  fi
  echo "inspect: python3 scripts/pcap.py $PCAP"
else
  echo "FAIL: no pcap generated"; fail=$((fail + 1))
fi

echo "net-test-tcp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

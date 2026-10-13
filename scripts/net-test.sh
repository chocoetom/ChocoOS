#!/usr/bin/env bash
# Passo 1 (net-1): ARP + IPv4 + ICMP.
# Boot com user-net virtio + filter-dump -> build/net.pcap, digita
# ifconfig/ping/arp no shell via monitor e confere o serial log.
# Uso: bash scripts/build-local.sh && bash scripts/net-test.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-net.log
PCAP=build/net.pcap
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

# NOTE: ioeventfd=off forces synchronous MMIO doorbells. With the default
# ioeventfd, kicks go through host eventfds; on this Windows QEMU build a
# kick to a dead backend (link down / no peer) stalls the vCPU forever.
# The MR path always returns, so TX timeouts stay guest-side and bounded.
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
  echo "FAIL: shell prompt never appeared"; kill "${Q_PID:-0}" 2>/dev/null || true; exit 1
fi

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 2.5
}

echo "[net-test] driving the shell..."
type_line "pci"
type_line "ifconfig"
type_line "arp"
type_line "ping 10.0.2.2 2"
type_line "arp flush"
type_line "ping 10.0.2.2 1"
type_line "arp"
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

check "[virtio] dev 1" "virtio-net probed (modern PCI caps)"
check "[virtio-net] MAC 52:54:00" "virtio-net MAC from device config"
check "[net] up: ip 10.0.2.15" "net stack up with static IP"
check "mac 52:54:00" "ifconfig shows MAC"
check "ip 10.0.2.15" "ifconfig shows IP"
check "reply seq=" "ping got ICMP reply from 10.0.2.2"
check "replies" "ping summary line"
check "arp:" "arp table printed"

echo "--- pcap ---"
if [ -f "$PCAP" ]; then
  echo "pcap: $PCAP ($(wc -c < "$PCAP") bytes)"
  if command -v tcpdump >/dev/null 2>&1; then
    tcpdump -r "$PCAP" -n 2>&1 | head -30
    if tcpdump -r "$PCAP" -n 2>/dev/null | grep -qi "arp"; then
      echo "PASS: pcap contains ARP"; pass=$((pass + 1))
    else
      echo "FAIL: pcap has no ARP"; fail=$((fail + 1))
    fi
    if tcpdump -r "$PCAP" -n 2>/dev/null | grep -qi "icmp"; then
      echo "PASS: pcap contains ICMP"; pass=$((pass + 1))
    else
      echo "FAIL: pcap has no ICMP"; fail=$((fail + 1))
    fi
  else
    python3 scripts/pcap.py "$PCAP"
    # The post-flush ping forces a fresh on-wire ARP exchange (the gateway
    # MAC is otherwise already known via snooping on the DHCP offer).
    if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "arp op=1"; then
      echo "PASS: pcap has ARP request"; pass=$((pass + 1))
    else
      echo "FAIL: pcap missing ARP request"; fail=$((fail + 1))
    fi
    if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "arp op=2"; then
      echo "PASS: pcap has ARP reply"; pass=$((pass + 1))
    else
      echo "FAIL: pcap missing ARP reply"; fail=$((fail + 1))
    fi
    if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "icmp type=8"; then
      echo "PASS: pcap has ICMP echo request"; pass=$((pass + 1))
    else
      echo "FAIL: pcap missing ICMP echo"; fail=$((fail + 1))
    fi
    if python3 scripts/pcap.py "$PCAP" 2>/dev/null | grep -q "icmp type=0"; then
      echo "PASS: pcap has ICMP echo reply"; pass=$((pass + 1))
    else
      echo "FAIL: pcap missing ICMP reply"; fail=$((fail + 1))
    fi
  fi
  echo "inspect: tcpdump -r $PCAP -n -e -v (or python3 scripts/pcap.py $PCAP)"
else
  echo "FAIL: no pcap generated"; fail=$((fail + 1))
fi

echo "net-test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

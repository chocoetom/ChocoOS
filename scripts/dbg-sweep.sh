#!/usr/bin/env bash
# Sweep QEMU variants to find one where the virtio backend processes TX.
# No typing: the init gratuitous ARP is enough signal (pcap size check).
# Usage: bash scripts/dbg-sweep.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
if [ ! -f "$ISO" ]; then echo "no ISO"; exit 1; fi
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1

run_variant() {
  local name="$1"; shift
  local log="build/sweep-${name}.log"
  local pcap="build/sweep-${name}.pcap"
  rm -f "$log" "$pcap"
  timeout 100 qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
    -serial file:"$log" -boot order=d -no-reboot \
    -netdev user,id=n0 -object filter-dump,id=dump0,netdev=n0,file="$pcap" \
    "$@" -monitor none >/dev/null 2>&1 &
  local qp=$!
  local ok=0
  for i in $(seq 1 95); do
    sleep 1
    if grep -q "choco> " "$log" 2>/dev/null; then ok=1; sleep 5; break; fi
  done
  kill "$qp" 2>/dev/null || true
  wait "$qp" 2>/dev/null || true
  local sz=0
  if [ -f "$pcap" ]; then sz=$(wc -c < "$pcap"); fi
  local net="?"
  if grep -q "net. up: ip" "$log" 2>/dev/null; then net="up"; else net="DOWN"; fi
  echo "variant=$name prompt=$ok net=$net pcap_bytes=$sz"
}

echo "=== sweep start ==="
run_variant base -accel tcg,tb-size=64 -device virtio-net-pci,netdev=n0,ioeventfd=off
run_variant single -accel tcg,tb-size=64,thread=single -device virtio-net-pci,netdev=n0,ioeventfd=off
run_variant multi -accel tcg,tb-size=64,thread=multi -device virtio-net-pci,netdev=n0,ioeventfd=off
run_variant q35 -machine q35 -accel tcg,tb-size=64 -device virtio-net-pci,netdev=n0,ioeventfd=off
run_variant modern -accel tcg,tb-size=64 -device virtio-net-pci,netdev=n0,ioeventfd=off,disable-legacy=on
run_variant novec -accel tcg,tb-size=64 -device virtio-net-pci,netdev=n0,ioeventfd=off,vectors=0
run_variant q35modern -machine q35 -accel tcg,tb-size=64 -device virtio-net-pci,netdev=n0,ioeventfd=off,disable-legacy=on
echo "=== sweep done ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true

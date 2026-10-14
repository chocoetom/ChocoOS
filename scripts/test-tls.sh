#!/usr/bin/env bash
# Headless E2E test for TLS: boots ChocoOS with a NIC, runs bin/httpsget against
# a host OpenSSL server, and checks that the handshake verifies and the body
# arrives. Proves: kernel CSPRNG -> getrandom, TCP connect, BearSSL handshake
# with X.509 chain validation, and the missing open(2) syscall.
#
# The server cert is the CA shipped at /etc/ssl/cacert.pem (self-signed), so a
# correct fetch and a name/CA mismatch are both exercised.
# Usage: bash scripts/build.sh && bash scripts/test-tls.sh
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"
need openssl "openssl"
need grep "coreutils"

ISO=build/ChocoOS.iso
[ -f "$ISO" ] || { echo "no ISO: build first"; exit 1; }
[ -f scripts/tls/server.pem ] || { echo "missing scripts/tls/server.pem (test cert)"; exit 1; }
LOG="build/serial.tls.$$.log"
PORT=18443
fresh "$LOG"
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1

# Host HTTPS server presenting the test cert, serving scripts/tls/www.
( cd scripts/tls/www && exec openssl s_server -accept 0.0.0.0:$PORT \
    -cert ../server.pem -key ../server.key -WWW -quiet ) >/dev/null 2>&1 &
SRV=$!
sleep 2

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -monitor stdio >/dev/null 2>&1; }

cleanup() {
  printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
  sleep 1
  kill "$SRV" 2>/dev/null || true
  taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
}
trap cleanup EXIT

for i in $(seq 1 90); do sleep 1; grep -q "choco> " "$LOG" 2>/dev/null && break; done
grep -q "choco> " "$LOG" 2>/dev/null || { echo "FAIL: shell prompt never appeared"; exit 1; }
# DHCP must have bound so the guest can reach 10.0.2.2.
for i in $(seq 1 20); do grep -q "dhcp] bound" "$LOG" 2>/dev/null && break; sleep 1; done

type_cmd() {
  local s="$1" k c
  for ((k = 0; k < ${#s}; k++)); do
    c="${s:$k:1}"
    case "$c" in
      ' ') c=spc ;; '/' ) c=slash ;; '.' ) c=dot ;; '-' ) c=minus ;;
      ':' ) c=shift-semicolon ;; '=' ) c=equal ;;
      [A-Z]) c="shift-$(echo "$c" | tr 'A-Z' 'a-z')" ;;
      [!a-z0-9]) c="" ;;
    esac
    [ -n "$c" ] && printf 'sendkey %s\n' "$c" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
}

PASS=0; FAIL=0
# run_cmd <command line> -> types it and streams the new serial output to stdout
run_cmd() {
  local n0; n0=$(wc -l < "$LOG" 2>/dev/null || echo 0)
  type_cmd "$1"
  sleep 6
  tail -n +$((n0 + 1)) "$LOG" 2>/dev/null
}

url="https://10.0.2.2:$PORT/index.html"
echo "[tls] positive: valid CA + matching name..."
out=$(run_cmd "run bin/httpsget $url /etc/ssl/cacert.pem localhost")
if echo "$out" | grep -qF "CHOCO-TLS-OK"; then
  echo "PASS: handshake + body over TLS"; PASS=$((PASS + 1))
else
  echo "FAIL: handshake + body over TLS"; FAIL=$((FAIL + 1))
fi

echo "[tls] negative: server name not in the certificate must fail..."
out=$(run_cmd "run bin/httpsget $url /etc/ssl/cacert.pem wrong.example")
if echo "$out" | grep -qF "CHOCO-TLS-OK"; then
  echo "FAIL: wrong server name still accepted"; FAIL=$((FAIL + 1))
elif echo "$out" | grep -qE "err=56|BAD_SERVER_NAME"; then
  echo "PASS: wrong server name rejected (X509 name check)"; PASS=$((PASS + 1))
elif echo "$out" | grep -qF "TLS ended"; then
  echo "PASS: wrong server name rejected (TLS error)"; PASS=$((PASS + 1))
else
  echo "FAIL: wrong server name produced no TLS error"; FAIL=$((FAIL + 1))
fi

echo "test-tls: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

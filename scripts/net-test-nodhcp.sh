#!/usr/bin/env bash
# Passo 2 (net-2), requisito 3: sem resposta DHCP, o boot NAO pode travar e
# a config de fallback (10.0.2.15) deve ser aplicada apos retries limitados.
# Tecnica: user-net normal, mas com o link derrubado via QMP antes do boot
# (nada entra ou sai) — testa timeouts, retries e fallback de ponta a ponta.
# Uso: bash scripts/build-local.sh && bash scripts/net-test-nodhcp.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin

ISO=build/ChocoOS.iso
LOG=build/serial-nodhcp.log
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1
rm -f "$LOG"

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
  -qmp tcp:127.0.0.1:4444,server=on,wait=off \
  -monitor stdio >/dev/null 2>build/qemu-nodhcp-stderr.log; }

sleep 2
python3 scripts/qmp_link.py down

# O boot precisa completar mesmo sem ninguem responder DHCP.
prompt_ok=0
for i in $(seq 1 90); do
  sleep 1
  if grep -q "choco> " "$LOG" 2>/dev/null; then prompt_ok=1; break; fi
done
if [ "$prompt_ok" -eq 0 ]; then
  echo "FAIL: boot travou sem DHCP (sem prompt)"; kill "${Q_PID:-0}" 2>/dev/null || true; exit 1
fi
echo "PASS: boot completou sem resposta DHCP"

# 4 discovers x 2s + margem: espera o fallback ser aplicado.
sleep 20

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
  sleep 2.5
}

type_line "dhcp"
type_line "ifconfig"
sleep 2
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true

pass=1; fail=0
check() {
  if grep -qF "$1" "$LOG" 2>/dev/null; then
    echo "PASS: $2"; pass=$((pass + 1))
  else
    echo "FAIL: $2 (missing '$1')"; fail=$((fail + 1))
  fi
}

check "no offer: keeping fallback" "retries esgotaram com mensagem clara"
check "dhcp: fallback" "estado final = fallback"
check "src fallback" "ifconfig mostra origem fallback"
check "ip 10.0.2.15" "fallback mantem o IP default"

echo "--- qemu stderr ---"
cat build/qemu-nodhcp-stderr.log 2>/dev/null || true
echo "net-test-nodhcp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

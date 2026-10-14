#!/usr/bin/env bash
# Headless regression runner for the Linux-ABI tests: boots the ISO once and
# runs every bin/l* program, checking each against an expected marker so a
# hang, panic or wrong result shows up as a FAIL.
#
# Needs the pieces the tests exercise: a NIC + a host HTTP server (lsock/lpoll)
# and a fresh FAT32 volume (lpersist). All are provisioned here.
# Usage: bash scripts/build.sh && bash scripts/run-ltests.sh
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"
need grep "coreutils"

ISO=build/ChocoOS.iso
[ -f "$ISO" ] || { echo "no ISO: build first"; exit 1; }
LOG="build/serial.ltests.$$.log"
VOL=build/chocovol-ltests.img
fresh "$LOG"
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
sleep 1

# Fresh FAT32 volume for lpersist.
if command -v mformat >/dev/null 2>&1; then
  dd if=/dev/zero of="$VOL" bs=512 count=65536 status=none
  mformat -i "$VOL" -F -v CHOCOVOL :: >/dev/null 2>&1
else
  echo "WARN: mformat missing; lpersist will likely fail"
fi
# Host HTTP server for the socket tests.
python3 -m http.server 18080 --directory docs >/dev/null 2>&1 &
SRV=$!

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -drive file="$VOL",format=raw,if=none,id=hdrvol \
  -device ide-hd,bus=ide.0,unit=0,drive=hdrvol \
  -monitor stdio >/dev/null 2>&1; }

cleanup() {
  printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
  sleep 1
  kill "$SRV" 2>/dev/null || true
  taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
  rm -f "$VOL"
}
trap cleanup EXIT

for i in $(seq 1 90); do sleep 1; grep -q "choco> " "$LOG" 2>/dev/null && break; done
grep -q "choco> " "$LOG" 2>/dev/null || { echo "FAIL: shell prompt never appeared"; exit 1; }

type_cmd() {
  local s="$1" k c
  for ((k = 0; k < ${#s}; k++)); do
    c="${s:$k:1}"
    case "$c" in ' ') c=spc ;; '/' ) c=slash ;; '.' ) c=dot ;; '-' ) c=minus ;; esac
    printf 'sendkey %s\n' "$c" >&"${Q[1]}"
    sleep 0.1
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
}

PASS=0; FAIL=0
# Tests known to be incomplete (their failure is expected, only a hang is a bug).
XFAIL=""
# run_one <test> <expected substring> <budget seconds>
run_one() {
  local t="$1" want="$2" budget="$3" n0
  n0=$(wc -l < "$LOG" 2>/dev/null || echo 0)
  type_cmd "run bin/$t"
  local end=$(( $(date +%s) + budget ))
  while [ "$(date +%s)" -lt "$end" ]; do
    tail -n +$((n0 + 1)) "$LOG" 2>/dev/null | grep -qF -- "$want" && break
    sleep 0.3
  done
  local seg
  seg=$(tail -n +$((n0 + 1)) "$LOG" 2>/dev/null)
  if echo "$seg" | grep -q "lying belly-up"; then
    echo "FAIL: $t (KERNEL PANIC)"; FAIL=$((FAIL + 1)); return
  fi
  if [[ " $XFAIL " == *" $t "* ]]; then
    if echo "$seg" | grep -qE "exit code|killed|killing task"; then
      echo "XFAIL: $t (known incomplete)"; PASS=$((PASS + 1))
    else
      echo "FAIL: $t (expected to terminate, hung)"; FAIL=$((FAIL + 1))
    fi
    return
  fi
  if echo "$seg" | grep -qF -- "$want"; then
    echo "PASS: $t"; PASS=$((PASS + 1))
  else
    echo "FAIL: $t (expected '$want')"; FAIL=$((FAIL + 1))
  fi
}

# test:expected-marker (grep -F). Fallback marker is a clean exit 0.
run_one lhello          "Hello from Linux ABI!"             4
run_one ltest           "ltest: PASS"                       4
run_one lstat           "stat: PASS"                        4
run_one lfstat          "fstat: PASS"                       4
run_one ldate           "date: PASS"                        4
run_one lmmap           "mmap: PASS"                        4
run_one lfswrite        "fs: PASS"                          4
run_one lmkdir          "mkdir: PASS"                       4
run_one lclone          "clone/wait: PASS"                  4
run_one lfork           "fork/wait: PASS"                   4
run_one lclonepipe      "clone/pipe/wait: PASS"             4
run_one lpipe           "exit code 0."                      4
run_one lpipebroken     "exit code 0."                      4
run_one lexec           "Hello from Linux ABI!"             4
run_one lexecclone      "Hello from Linux ABI!"             4
run_one lguard          "lguard: pointer-guard"             4
run_one lhandler        "handler: PASS"                     4
run_one lsigconfig      "exit code 0."                      4
run_one ltty            "tty"                               4
run_one lttygrp         "exit code 0."                      4
run_one lhier           "exit code 0."                      4
run_one lbusyexecpipe   "busyexec-ok"                      8
run_one lbusypipe       "busypipe: PASS"                  10
run_one lfpu            "fpu: PASS"                       12
run_one lkillself       "exit code 143."                    4
run_one lpersist        "first boot"                       10
run_one lrand           "rand: PASS"                        4
run_one lmemfd          "memfd: PASS"                       4
run_one lscm            "scm: PASS"                         4
run_one wlrun           "wlrun: composite OK"               8
run_one lsock           "sock: PASS"                       10
run_one lpoll           "poll: PASS"                       10
run_one lsp             "spair: PASS"                       4
run_one lusock          "usock: PASS"                       4
run_one lfreetype       "ftfont: PASS"                      20

echo "run-ltests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

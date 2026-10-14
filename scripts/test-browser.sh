#!/usr/bin/env bash
# Headless end-to-end smoke for /bin/browser.
#
# Boots the ISO with a virtio NIC, serves a fixture page from the host
# (127.0.0.1:18081 -> guest 10.0.2.2:18081), launches `run bin/browser <url>`,
# then asserts on a framebuffer screendump that the app fetched the page,
# parsed a subset of HTML and painted formatted text: a link (blue) and a
# heading (dark, enlarged) can only appear if the fetch + parser + layout ran.
#
# Usage: bash scripts/build-local.sh && bash scripts/test-browser.sh
set -u
cd "$(dirname "$0")/.."
. scripts/host-env.sh
need qemu-system-x86_64 "mingw-w64-ucrt-x86_64-qemu-system-native"
need python3 "python"

ISO=build/ChocoOS.iso
RUN=$$
LOG=build/serial-browser.$RUN.log
SHOT=build/browser_root.$RUN.ppm
PORT=8080
if [ ! -f "$ISO" ]; then echo "no ISO: build first"; exit 1; fi

fresh "$LOG"
fresh "$SHOT"

# Fixture HTTP server on the host loopback. The browser's default page is
# http://10.0.2.2:8080/ (guest 10.0.2.2 -> host 127.0.0.1), so we launch it
# with no argument and avoid typing ':' through the QEMU monitor.
python3 -m http.server "$PORT" --directory scripts/browser-root --bind 127.0.0.1 \
  >build/browser-http.log 2>&1 &
SRV=$!
sleep 2

sk() {
  case "$1" in
    [a-z0-9]) echo "$1" ;;
    ' ') echo spc ;; '/' ) echo slash ;; '.' ) echo dot ;; '-' ) echo minus ;;
    ':' ) echo colon ;;
    [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')" ;;
  esac
}

coproc Q { exec qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none \
  -accel tcg,tb-size=64 -serial file:"$LOG" -boot order=d -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,ioeventfd=off \
  -monitor stdio >/dev/null 2>&1; }

for i in $(seq 1 60); do
  sleep 1
  grep -q "choco> " "$LOG" 2>/dev/null && break
done
if ! grep -q "choco> " "$LOG" 2>/dev/null; then
  echo "FAIL: shell prompt never appeared"
  kill "$SRV" 2>/dev/null || true
  [ -n "${Q_PID:-}" ] && kill "$Q_PID" 2>/dev/null || true
  exit 1
fi

sleep 12  # let DHCP bring the interface up

type_line() {
  local s="$1" k
  for ((k = 0; k < ${#s}; k++)); do
    printf 'sendkey %s\n' "$(sk "${s:$k:1}")" >&"${Q[1]}"
    sleep 0.12
  done
  printf 'sendkey ret\n' >&"${Q[1]}"
}

echo "[test-browser] launching the browser (default page: 10.0.2.2:$PORT)..."
type_line "run bin/browser"

# The fetch is a blocking syscall; give it time to connect, receive, parse and
# lay out, then let the compositor paint at least one frame.
sleep 12
printf 'screendump %s\n' "$SHOT" >&"${Q[1]}"
sleep 2
printf 'quit\n' >&"${Q[1]}" 2>/dev/null || true
sleep 1
kill "$SRV" 2>/dev/null || true

pass=0; fail=0
check() { if [ "$1" = "1" ]; then echo "PASS: $2"; pass=$((pass + 1)); else echo "FAIL: $2"; fail=$((fail + 1)); fi; }

count_color() {
  if command -v python3 >/dev/null 2>&1; then PY=python3
  elif command -v python >/dev/null 2>&1; then PY=python
  else PY=/c/Python314/python.exe; fi
  "$PY" scripts/ppm_count.py "$1" "$2"
}

if [ ! -f "$SHOT" ]; then
  echo "FAIL: missing screendump (did the window open?)"; exit 1
fi

white=$(count_color "$SHOT" FFFFFF)     # page "paper" background
link=$(count_color "$SHOT" 1D6FBF)      # <a> accent -> proves links parsed
head=$(count_color "$SHOT" 241F3A)      # <h1>/<h2> ink -> proves headings parsed
chrome=$(count_color "$SHOT" 1A1927)    # toolbar surface -> proves the window drew

check "$([ "${white:-0}" -gt 2000 ] && echo 1 || echo 0)" \
      "browser window painted a page canvas (white: ${white} px)"
check "$([ "${chrome:-0}" -gt 0 ] && echo 1 || echo 0)" \
      "toolbar/chrome rendered (surface: ${chrome} px)"
check "$([ "${link:-0}" -gt 0 ] && echo 1 || echo 0)" \
      "HTML links fetched + rendered (link color: ${link} px)"
check "$([ "${head:-0}" -gt 0 ] && echo 1 || echo 0)" \
      "HTML headings rendered with formatting (heading ink: ${head} px)"

# A negative signal: the connection-failure message means fetch did not work.
if grep -aqi "could not connect" "$LOG" 2>/dev/null; then
  check 0 "did not hit a connection error"
else
  check 1 "no 'could not connect' error"
fi

echo "test-browser: $pass passed, $fail failed"
[ "$fail" -eq 0 ] && rm -f "$LOG" "$SHOT" build/browser-http.log
exit $((fail == 0 ? 0 : 1))

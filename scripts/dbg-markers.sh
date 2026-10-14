#!/usr/bin/env bash
# Check my markers are present in the worktree.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin
check() { # pattern file
  n=$(grep -c "$1" "$2" 2>/dev/null || true)
  echo "$2 [$1]: $n"
}
check "c_dhcp" kernel/shell.cpp
check "c_udpstat" kernel/shell.cpp
check "send_ipv4_from" kernel/net/net.cpp
check "recv_from" kernel/net/udp.cpp
check "run_once" kernel/net/dhcp.cpp
check "dhcp::start" kernel/main.cpp
check "dhcp.cpp" CMakeLists.txt
check "link down" kernel/proc/virtio_net.cpp
check "c_stress" kernel/shell.cpp
check "sleep_ms" kernel/cpu/sched.cpp

#!/usr/bin/env bash
# Report -Wall warnings for kernel/net only.
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin
touch kernel/net/dns.cpp kernel/net/udp.cpp kernel/net/dhcp.cpp kernel/net/net.cpp
cmake --build build 2>&1 | grep -a -B1 -A3 -i warning | head -30

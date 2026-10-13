#!/usr/bin/env bash
# Where is the stuck RIP? args: elf addr
set -u
cd "$(dirname "$0")/.."
export PATH=/ucrt64/bin:/usr/bin:/bin
ELF=${1:-build/boot/chocoos.elf}
ADDR=${2:-0xffffffff80006bb0}
nm -n "$ELF" > build/syms.txt
python3 - "$ELF" "$ADDR" <<'EOF'
import sys
addr = int(sys.argv[2], 16)
best = None
with open("build/syms.txt") as f:
    for line in f:
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            a = int(parts[0], 16)
        except ValueError:
            continue
        if a <= addr:
            best = (a, parts[1], parts[2])
print("RIP 0x%x -> %s+0x%x (%s)" % (addr, best[2], addr - best[0], best[1]))
EOF

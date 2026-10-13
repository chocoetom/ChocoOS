#!/usr/bin/env python3
# Raw hexdump of pcap packets.
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "build/dbg-trace.pcap"
d = open(path, "rb").read()
off = 24
n = 0
while off + 16 <= len(d):
    ts_sec, ts_usec, incl, orig = struct.unpack("<IIII", d[off : off + 16])
    pkt = d[off + 16 : off + 16 + incl]
    off += 16 + incl
    n += 1
    print(f"pkt{n} incl={incl} orig={orig} data:")
    for i in range(0, len(pkt), 16):
        chunk = pkt[i : i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  {i:04x}  {hexs:<47}  {asc}")

#!/usr/bin/env python3
# Minimal pcap reader: prints linktype + per-packet ethertype/IP summary.
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "build/dbg-trace.pcap"
d = open(path, "rb").read()
if len(d) < 24:
    print("too small")
    sys.exit(1)
magic, ver_maj, ver_min, _, _, _, linktype = struct.unpack("<IHHIIII", d[:24])
print(f"magic={magic:08x} linktype={linktype} bytes={len(d)}")
off = 24
n = 0
while off + 16 <= len(d):
    ts_sec, ts_usec, incl, orig = struct.unpack("<IIII", d[off : off + 16])
    pkt = d[off + 16 : off + 16 + incl]
    off += 16 + incl
    n += 1
    if len(pkt) < 14:
        print(f"pkt{n} len={incl} runt")
        continue
    dst = ":".join(f"{b:02x}" for b in pkt[0:6])
    src = ":".join(f"{b:02x}" for b in pkt[6:12])
    et = (pkt[12] << 8) | pkt[13]
    extra = ""
    if et == 0x0806 and len(pkt) >= 42:
        op = (pkt[20] << 8) | pkt[21]
        spa = ".".join(str(b) for b in pkt[28:32])
        tpa = ".".join(str(b) for b in pkt[38:42])
        extra = f"arp op={op} spa={spa} tpa={tpa}"
    elif et == 0x0800 and len(pkt) >= 34:
        proto = pkt[23]
        sip = ".".join(str(b) for b in pkt[26:30])
        dip = ".".join(str(b) for b in pkt[30:34])
        extra = f"ip proto={proto} {sip}->{dip}"
        if proto == 1 and len(pkt) >= 42:
            extra += f" icmp type={pkt[34]} code={pkt[35]}"
    print(f"pkt{n} len={incl} {src}->{dst} et=0x{et:04x} {extra}")
print(f"total {n} packets")

#!/usr/bin/env python3
# Minimal pcap reader: prints linktype + per-packet ethertype/IP summary,
# with UDP and DHCP (ports 67/68) decoding for the net-2 DHCP test.
import struct
import sys

DHCP_TYPES = {1: "discover", 2: "offer", 3: "request", 4: "decline",
              5: "ack", 6: "nak", 7: "release", 8: "inform"}


def decode_dhcp(udp, out):
    # udp: full UDP datagram bytes. Returns "dhcp <type> ..." or "".
    if len(udp) < 240:
        return ""
    if udp[236:240] != bytes([99, 130, 83, 99]):
        return ""
    op = udp[0]
    xid = struct.unpack(">I", udp[4:8])[0]
    yiaddr = ".".join(str(b) for b in udp[16:20])
    msg, server = 0, ""
    p = 240
    while p < len(udp):
        tag = udp[p]
        p += 1
        if tag == 0:
            continue
        if tag == 255:
            break
        if p >= len(udp):
            break
        ln = udp[p]
        p += 1
        if p + ln > len(udp):
            break
        if tag == 53 and ln == 1:
            msg = udp[p]
        if tag == 54 and ln == 4:
            server = ".".join(str(b) for b in udp[p:p + 4])
        p += ln
    kind = DHCP_TYPES.get(msg, f"type{msg}")
    return f"dhcp {kind} op={op} xid={xid:08x} yiaddr={yiaddr} server={server}"


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
        if proto == 17 and len(pkt) >= 42:
            sport = (pkt[34] << 8) | pkt[35]
            dport = (pkt[36] << 8) | pkt[37]
            ulen = (pkt[38] << 8) | pkt[39]
            extra += f" udp {sport}->{dport} len={ulen}"
            if (sport in (67, 68) or dport in (67, 68)) and ulen >= 248:
                ud = pkt[34:34 + ulen]
                if len(ud) >= 248:
                    dd = decode_dhcp(ud[8:], extra)
                    if dd:
                        extra += f" {dd}"
    print(f"pkt{n} len={incl} {src}->{dst} et=0x{et:04x} {extra}")
print(f"total {n} packets")

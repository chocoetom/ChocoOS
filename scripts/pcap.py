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


def decode_dns_name(msg, pos):
    # Returns (name, next_pos) or (None, pos) on bounds failure. Follows
    # compression pointers with a jump cap (mirrors the kernel skip_name).
    labels = []
    jumps = 0
    end = None  # linear position past the name (after 1st pointer/zero)
    while True:
        if pos >= len(msg):
            return None, end if end is not None else pos
        b = msg[pos]
        if b == 0:
            if end is None:
                end = pos + 1
            break
        if (b & 0xC0) == 0xC0:
            if pos + 1 >= len(msg):
                return None, end if end is not None else pos
            tgt = ((b & 0x3F) << 8) | msg[pos + 1]
            if tgt >= len(msg):
                return None, end if end is not None else pos
            if end is None:
                end = pos + 2
            jumps += 1
            if jumps > 8:
                return None, end if end is not None else pos
            pos = tgt
            continue
        if (b & 0xC0) or b > 63 or pos + 1 + b > len(msg):
            return None, end if end is not None else pos
        labels.append("".join(chr(c) for c in msg[pos + 1:pos + 1 + b]))
        pos += 1 + b
    return ".".join(labels), end


def decode_dns(udp):
    # udp: full UDP datagram. Returns "dns ..." summary or "".
    if len(udp) < 12:
        return ""
    did = (udp[0] << 8) | udp[1]
    flags = (udp[2] << 8) | udp[3]
    qd = (udp[4] << 8) | udp[5]
    an = (udp[6] << 8) | udp[7]
    rcode = flags & 0xF
    if flags & 0x8000:
        kind = "resp"
    else:
        kind = "query"
    pos = 12
    qname = "?"
    if qd > 0:
        qname, pos = decode_dns_name(udp, pos)
        if qname is None:
            return ""
        pos += 4
    out = f"dns {kind} id={did:04x} rcode={rcode} q={qname} an={an}"
    if an > 0 and flags & 0x8000:
        ips = []
        for _ in range(min(an, 8)):
            nm, pos = decode_dns_name(udp, pos)
            if nm is None or pos + 10 > len(udp):
                break
            rtype = (udp[pos] << 8) | udp[pos + 1]
            rdlen = (udp[pos + 8] << 8) | udp[pos + 9]
            pos += 10
            if pos + rdlen > len(udp):
                break
            if rtype == 1 and rdlen == 4:
                ips.append(".".join(str(b) for b in udp[pos:pos + 4]))
            pos += rdlen
        if ips:
            out += " a=" + ",".join(ips)
    return out


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
            ud = pkt[42:42 + max(0, ulen - 8)]
            if {sport, dport} == {67, 68} and len(ud) >= 240:
                dd = decode_dhcp(ud, extra)
                if dd:
                    extra += f" {dd}"
            elif (sport == 53 or dport == 53) and len(ud) >= 12:
                dd = decode_dns(ud)
                if dd:
                    extra += f" {dd}"
    print(f"pkt{n} len={incl} {src}->{dst} et=0x{et:04x} {extra}")
print(f"total {n} packets")

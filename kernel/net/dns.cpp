// ChocoOS DNS client (UDP/53, A records only) — net-3.
//
// One synchronous query with bounded retries; strict parsing because every
// response byte is untrusted input from the network.
//
// BOUNDS AUDIT:
//  - (E1) QNAME encoding refuses names >253 chars or labels >63 (RFC 1035).
//  - (E2) query/response fit fixed stack buffers (query <= 512, resp 512).
//  - (E3) response must hold the 12-byte header; ID/QR/OPCODE checked.
//  - (E4) RCODE 3 (NXDOMAIN) returns -3 immediately; other nonzero RCODEs
//    fail the attempt (retry).
//  - (E5) question/answer names skipped with compression-aware skip_name():
//    label lengths and pointer targets validated against the message end,
//    pointer jumps capped at 8 (no cycles), total skips bounded.
//  - (E6) each RR needs 10 bytes of fixed fields; RDLEN validated against
//    the remaining message before RDATA is touched.
//  - (E7) at most 32 answer RRs examined; at most max_ips stored.
//  - (E8) attempts x per-attempt timeout bound the total block time.
#include <choco/console.hpp>
#include <choco/net.hpp>
#include <choco/util.hpp>

#include "net_internal.hpp"

using choco::console::BG;
using choco::console::FG;
namespace internal = choco::net::internal;

namespace choco::net::dns {
namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr uint16_t QTYPE_A = 1;
constexpr uint16_t QCLASS_IN = 1;
constexpr int MAX_ATTEMPTS = 3;
constexpr uint16_t MAX_NAME_WIRE = 255;
constexpr int MAX_JUMPS = 8;
constexpr int MAX_RR = 32;

uint32_t s_qid = 0xD515u;

// Encode "example.com." into DNS wire format at out (no trailing root yet).
// Returns wire length including the root zero, or 0 when invalid (E1).
uint16_t encode_name(const char *name, uint8_t *out) {
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    while (nlen > 0 && name[nlen - 1] == '.') nlen--;  // tolerate FQDN dot
    if (nlen == 0 || nlen > 253) return 0;
    size_t o = 0, label_start = 0, label_len = 0;
    for (size_t i = 0; i <= nlen; i++) {
        char c = (i < nlen) ? name[i] : '.';
        bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!valid) return 0;
        if (c == '.') {
            if (label_len == 0 || label_len > 63) return 0;
            if (o + 1 + label_len > MAX_NAME_WIRE) return 0;
            out[o++] = (uint8_t)label_len;
            for (size_t k = 0; k < label_len; k++) out[o++] = (uint8_t)name[label_start + k];
            label_start = i + 1;
            label_len = 0;
        } else {
            label_len++;
        }
    }
    if (o + 1 > MAX_NAME_WIRE + 1) return 0;
    out[o++] = 0;  // root
    return (uint16_t)o;
}

// Skip a possibly-compressed name at pos. Returns offset past it, or -1.
// (E5) every read bounds-checked; pointer cycles capped.
int skip_name(const uint8_t *msg, uint16_t msg_len, uint16_t pos) {
    uint16_t next = 0;  // return position after the first pointer/zero
    int jumps = 0;
    for (;;) {
        if (pos >= msg_len) return -1;
        uint8_t b = msg[pos];
        if (b == 0) {
            return next ? next : (int)pos + 1;
        }
        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= msg_len) return -1;
            uint16_t target = ((uint16_t)(b & 0x3F) << 8) | msg[pos + 1];
            if (target >= msg_len) return -1;
            if (!next) next = pos + 2;
            if (++jumps > MAX_JUMPS) return -1;
            pos = target;
            continue;
        }
        if (b & 0xC0) return -1;  // reserved label types 01/10
        if (b > 63) return -1;
        if ((uint32_t)pos + 1u + b > msg_len) return -1;
        pos = (uint16_t)(pos + 1 + b);
    }
}

uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

}  // namespace

int resolve(const char *name, uint32_t *out_ips, int max_ips, uint32_t timeout_ms) {
    if (!name || !out_ips || max_ips <= 0) return -2;
    if (!ready()) return -1;
    uint32_t server = dns_server();
    if (server == 0) return -1;

    static uint8_t query[512];
    uint8_t qname[MAX_NAME_WIRE + 1];
    uint16_t qn = encode_name(name, qname);  // (E1)
    if (qn == 0) return -2;
    if ((uint32_t)12 + qn + 4 > sizeof(query)) return -2;  // (E2)

    uint16_t sport = 0;
    for (int t = 0; t < 8; t++) {  // grab a free ephemeral port
        uint16_t cand = (uint16_t)(40000 + ((pit::ticks() + (uint32_t)t * 7919u) % 20000u));
        if (udp::bind(cand)) {
            sport = cand;
            break;
        }
    }
    if (!sport) return -1;

    // ARP prelude: the query can't go out while the server's MAC is unknown,
    // and a blind send would burn an attempt (send drops on ARP miss).
    // Same-subnet servers ARP directly, off-subnet via the gateway.
    uint32_t srv = server;
    uint32_t next = ((srv & mask()) == (ip() & mask())) ? srv : gw();
    if (!internal::arp_resolve(next, 2000)) return -1;

    int found = 0;
    int rc = -1;  // default: timeout
    bool done = false;
    uint32_t per_try = timeout_ms / MAX_ATTEMPTS;
    if (per_try < 1000) per_try = 1000;
    for (int att = 0; att < MAX_ATTEMPTS && !done; att++) {
        uint16_t id = (uint16_t)(++s_qid + pit::ticks());
        size_t o = 0;
        query[o++] = (uint8_t)(id >> 8);
        query[o++] = (uint8_t)id;
        query[o++] = 0x01;  // RD
        query[o++] = 0x00;
        query[o++] = 0x00;
        query[o++] = 0x01;  // QDCOUNT=1
        query[o++] = 0x00;
        query[o++] = 0x00;  // ANCOUNT=0
        query[o++] = 0x00;
        query[o++] = 0x00;  // NSCOUNT=0
        query[o++] = 0x00;
        query[o++] = 0x00;  // ARCOUNT=0
        for (uint16_t i = 0; i < qn; i++) query[o++] = qname[i];
        query[o++] = 0x00;
        query[o++] = QTYPE_A;  // QTYPE=A
        query[o++] = 0x00;
        query[o++] = QCLASS_IN;  // QCLASS=IN
        uint16_t qlen = (uint16_t)o;
        udp::send_to(ip(), sport, server, DNS_PORT, query, qlen);
        // NOTE: no early-out on send failure; the wait below still gives a
        // late ARP reply a chance, and every path stays time-bounded.

        static uint8_t resp[512];  // (E2) Slirp truncates bigger answers to fit
        uint64_t t0 = internal::now_ms();
        while (internal::now_ms() - t0 < per_try) {
            uint32_t from_ip = 0;
            uint16_t from_port = 0;
            int n = udp::recv_from(sport, resp, sizeof(resp), 250, &from_ip, &from_port);
            if (n < 0) continue;
            if (from_port != DNS_PORT) continue;
            if (n < 12) continue;  // (E3) header must fit
            uint16_t rid = get16(resp);
            if (rid != id) continue;
            uint16_t flags = get16(resp + 2);
            if (!(flags & 0x8000)) continue;   // QR must be response
            if (flags & 0x7800) continue;      // OPCODE must be 0
            uint8_t rcode = flags & 0x000F;
            if (rcode == 3) {
                rc = -3;
                done = true;  // NXDOMAIN is definitive
                break;
            }
            if (rcode != 0) continue;  // (E4) retry other errors
            uint16_t qd = get16(resp + 4);
            uint16_t an = get16(resp + 6);
            uint16_t pos = 12;
            bool ok = true;
            for (uint16_t q = 0; q < qd; q++) {  // skip questions
                int np = skip_name(resp, (uint16_t)n, pos);
                if (np < 0 || (uint32_t)np + 4 > (uint32_t)n) {
                    ok = false;
                    break;
                }
                pos = (uint16_t)np + 4;
            }
            if (!ok) continue;
            int examined = 0;
            while (an-- > 0 && examined < MAX_RR) {  // (E7) cap work
                examined++;
                int np = skip_name(resp, (uint16_t)n, pos);  // (E5)
                if (np < 0) {
                    ok = false;
                    break;
                }
                pos = (uint16_t)np;
                if ((uint32_t)pos + 10 > (uint32_t)n) {  // (E6) fixed fields
                    ok = false;
                    break;
                }
                uint16_t type = get16(resp + pos);
                uint16_t cls = get16(resp + pos + 2);
                uint16_t rdlen = get16(resp + pos + 8);
                pos += 10;
                if ((uint32_t)pos + rdlen > (uint32_t)n) {  // (E6) rdata
                    ok = false;
                    break;
                }
                if (type == QTYPE_A && cls == QCLASS_IN && rdlen == 4 &&
                    found < max_ips) {
                    out_ips[found++] = internal::ip_of(resp + pos);
                }
                pos += rdlen;
            }
            if (!ok) continue;
            rc = found;  // may be 0 with RCODE 0 and no A records
            done = true;
            break;
        }
    }
    udp::unbind(sport);
    return rc < 0 ? rc : found;
}

}  // namespace choco::net::dns

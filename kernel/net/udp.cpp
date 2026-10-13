// ChocoOS UDP (RFC 768) — minimal socket table for DHCP (client port 68)
// and later DNS (net-3).
//
// BOUNDS AUDIT (requirement: every network read is length-checked):
//  - dispatch(): drops unless dgram holds the full 8-byte header (A1),
//    then clamps the wire length field to the actual bytes (A2), drops
//    payloads over MAX_DGRAM (A3), drops unknown/closed ports (A4), and
//    verifies a nonzero checksum over pseudo-header+datagram (A5).
//  - recv_from(): copies at most max bytes into the caller buffer (B1),
//    never blocks past timeout_ms (B2).
//  - send_to(): refuses payloads over MAX_DGRAM so IP stays unfragmented
//    (C1); builds header+checksum in a fixed stack buffer (C2).
// Locking: table + RX slots are touched under internal::Lock (dispatch runs
// inside net::poll's lock; public calls lock around table access).
#include <choco/console.hpp>
#include <choco/net.hpp>
#include <choco/sched.hpp>
#include <choco/util.hpp>

#include "net_internal.hpp"

using choco::console::BG;
using choco::console::FG;
namespace internal = choco::net::internal;

namespace choco::net::udp {
namespace {

#pragma pack(push, 1)
struct Hdr {
    uint16_t sport;  // big-endian
    uint16_t dport;  // big-endian
    uint16_t len;    // big-endian, header + payload
    uint16_t csum;   // big-endian, 0 = not used
};
#pragma pack(pop)

constexpr int MAX_SOCK = 8;
constexpr uint16_t SLOT_SZ = 1536;

struct Sock {
    bool used = false;
    uint16_t port = 0;
    bool has_pkt = false;
    uint16_t pkt_len = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    uint8_t slot[SLOT_SZ]{};
};
Sock s_sock[MAX_SOCK]{};
SockStats s_stats{};

Sock *find_locked(uint16_t port) {
    for (int i = 0; i < MAX_SOCK; i++)
        if (s_sock[i].used && s_sock[i].port == port) return &s_sock[i];
    return nullptr;
}

// UDP checksum over pseudo-header + datagram (csum field zeroed).
// dgram_len is the WIRE-clamped length (<= MAX_DGRAM + HDR_SZ).
uint16_t tx_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *dgram, uint16_t dgram_len) {
    uint8_t pseudo[12];
    internal::ip_to_bytes(src_ip, pseudo);
    internal::ip_to_bytes(dst_ip, pseudo + 4);
    pseudo[8] = 0;
    pseudo[9] = 17;
    pseudo[10] = (uint8_t)(dgram_len >> 8);
    pseudo[11] = (uint8_t)(dgram_len & 0xFF);
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += ((uint32_t)pseudo[i] << 8) | pseudo[i + 1];
    for (uint16_t i = 0; i + 1 < dgram_len; i += 2)
        sum += ((uint32_t)dgram[i] << 8) | dgram[i + 1];
    if (dgram_len & 1) sum += (uint32_t)dgram[dgram_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return c == 0 ? 0xFFFF : c;  // RFC 768: 0 transmitted as all-ones
}

}  // namespace

bool bind(uint16_t port) {
    if (port == 0) return false;
    internal::Lock guard;
    if (find_locked(port)) return false;
    for (int i = 0; i < MAX_SOCK; i++) {
        if (!s_sock[i].used) {
            s_sock[i].used = true;
            s_sock[i].port = port;
            s_sock[i].has_pkt = false;
            s_sock[i].pkt_len = 0;
            return true;
        }
    }
    return false;
}

void unbind(uint16_t port) {
    internal::Lock guard;
    Sock *s = find_locked(port);
    if (s) {
        s->used = false;
        s->has_pkt = false;
    }
}

bool send_to(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
             const void *data, uint16_t len) {
    if (!data && len) return false;
    if (len > MAX_DGRAM) return false;  // (C1) keep IP unfragmented
    internal::Lock guard;  // re-entrant with send_ipv4_from's lock
    static uint8_t dgram[HDR_SZ + MAX_DGRAM];  // (C2) fixed buffer
    Hdr *h = (Hdr *)dgram;
    h->sport = internal::bswap16(src_port);
    h->dport = internal::bswap16(dst_port);
    h->len = internal::bswap16((uint16_t)(HDR_SZ + len));
    h->csum = 0;
    if (len) util::memcpy(dgram + HDR_SZ, data, len);
    uint16_t c = tx_checksum(src_ip, dst_ip, dgram, (uint16_t)(HDR_SZ + len));
    h->csum = internal::bswap16(c);
    if (!send_ipv4_from(src_ip, dst_ip, 17, dgram, (uint16_t)(HDR_SZ + len))) return false;
    s_stats.tx++;
    return true;
}

// Called by handle_ipv4 (already under net::poll's lock). dst_ip is the
// packet's destination (needed for the pseudo-header checksum check;
// may differ from our address while DHCP is unbound).
void dispatch(uint32_t src_ip, uint32_t dst_ip, const uint8_t *dgram, uint16_t dgram_len) {
    // (A1) header must fit.
    if (dgram_len < HDR_SZ) {
        s_stats.dropped++;
        return;
    }
    const Hdr *h = (const Hdr *)dgram;
    uint16_t wire_len = internal::bswap16(h->len);
    // (A2) wire length must cover the header and fit the actual bytes.
    if (wire_len < HDR_SZ || wire_len > dgram_len) {
        s_stats.dropped++;
        return;
    }
    dgram_len = wire_len;
    uint16_t payload_len = dgram_len - HDR_SZ;
    // (A3) payload cap (keeps slot copy bounded).
    if (payload_len > MAX_DGRAM) {
        s_stats.dropped++;
        return;
    }
    uint16_t sport = internal::bswap16(h->sport);
    uint16_t dport = internal::bswap16(h->dport);
    Sock *s = find_locked(dport);
    // (A4) no socket -> drop (no ICMP port-unreachable in this phase).
    if (!s) {
        s_stats.dropped++;
        return;
    }
    // (A5) verify nonzero checksums (pseudo-header + datagram).
    uint16_t wire_csum = internal::bswap16(h->csum);
    if (wire_csum != 0) {
        static uint8_t tmp[HDR_SZ + MAX_DGRAM];
        util::memcpy(tmp, dgram, dgram_len);
        tmp[6] = 0;
        tmp[7] = 0;
        if (tx_checksum(src_ip, dst_ip, tmp, dgram_len) != wire_csum) {
            s_stats.csum_err++;
            s_stats.dropped++;
            return;
        }
    }
    if (s->has_pkt) {
        s_stats.dropped++;  // single-slot socket is full; keep the older packet
        return;
    }
    if (payload_len > SLOT_SZ) {
        s_stats.dropped++;
        return;
    }
    util::memcpy(s->slot, dgram + HDR_SZ, payload_len);
    s->pkt_len = payload_len;
    s->from_ip = src_ip;
    s->from_port = sport;
    s->has_pkt = true;
    s_stats.rx++;
}

bool try_take(uint16_t port, void *buf, uint16_t max, uint16_t &out_len, uint32_t &from_ip,
              uint16_t &from_port) {
    internal::Lock guard;
    Sock *s = find_locked(port);
    if (!s || !s->has_pkt) return false;
    // (B1) clamp to caller buffer.
    uint16_t n = s->pkt_len < max ? s->pkt_len : max;
    util::memcpy(buf, s->slot, n);
    out_len = n;
    from_ip = s->from_ip;
    from_port = s->from_port;
    s->has_pkt = false;
    return true;
}

int recv_from(uint16_t port, void *buf, uint16_t max, uint32_t timeout_ms, uint32_t *from_ip,
              uint16_t *from_port) {
    if (!buf || max == 0) return -1;
    uint64_t t0 = internal::now_ms();
    for (;;) {
        uint16_t got = 0;
        uint32_t fip = 0;
        uint16_t fport = 0;
        if (try_take(port, buf, max, got, fip, fport)) {
            if (from_ip) *from_ip = fip;
            if (from_port) *from_port = fport;
            return got;
        }
        // (B2) bounded wait; poll() forwards RX into socket slots.
        // NOTE: sleep granularity is 10ms (sleep_ms(<10) rounds to zero ticks
        // and would busy-loop: the thread must yield so others run even when
        // no packets (and no timer IRQs) arrive.
        if (internal::now_ms() - t0 >= timeout_ms) return -1;
        poll();
        sched::sleep_ms(10);
    }
}

int socket_count() {
    internal::Lock guard;
    int n = 0;
    for (int i = 0; i < MAX_SOCK; i++)
        if (s_sock[i].used) n++;
    return n;
}

const SockStats &stats() { return s_stats; }

}  // namespace choco::net::udp

// ChocoOS net stack: Ethernet + ARP + IPv4 + ICMP (net-1), UDP demux hookup
// and dynamic config for DHCP (net-2). Locking: see net_internal.hpp.
#include <choco/console.hpp>
#include <choco/net.hpp>
#include <choco/paging.hpp>
#include <choco/sched.hpp>
#include <choco/serial.hpp>
#include <choco/util.hpp>
#include <choco/virtio_net.hpp>

#include "net_internal.hpp"

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;
namespace internal = choco::net::internal;

namespace choco::net {
namespace {

#pragma pack(push, 1)
struct Eth {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;  // big-endian
};
struct ArpPkt {
    uint16_t htype;  // 1 = ethernet
    uint16_t ptype;  // 0x0800 = ipv4
    uint8_t hlen;    // 6
    uint8_t plen;    // 4
    uint16_t op;     // 1=request 2=reply
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
};
struct IpHdr {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total;  // big-endian
    uint16_t id;
    uint16_t frag;  // big-endian
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint8_t src[4];
    uint8_t dst[4];
};
struct IcmpHdr {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
};
struct UdpHdr {
    uint16_t sport;  // big-endian
    uint16_t dport;  // big-endian
    uint16_t len;    // big-endian, header+payload
    uint16_t csum;   // big-endian, 0 = none
};
#pragma pack(pop)

constexpr uint16_t ETH_ARP = 0x0806;
constexpr uint16_t ETH_IP = 0x0800;
constexpr uint8_t IP_ICMP = 1;
constexpr uint8_t IP_TCP = 6;
constexpr uint8_t IP_UDP = 17;
constexpr uint32_t IP_BCAST_ALL = 0xFFFFFFFFu;

virtio_net::NetDevice s_dev{};
bool s_ready = false;
Stats s_stats{};

// Dynamic config. Written once by the DHCP thread on bind (or never, keeping
// boot defaults); read everywhere else. Single-copy u32/byte access is atomic
// on x86, so plain reads are safe without the lock (eventual consistency).
uint32_t s_ip = IP_GUEST;
uint32_t s_mask = IP_MASK;
uint32_t s_gw = IP_GW;
uint32_t s_dns = IP_DNS;
IpSource s_source = IpSource::DEFAULT;

uint16_t s_ip_id = 0xC40C;
uint16_t s_ping_id = 0xC40C;
uint16_t s_ping_seq = 0;
volatile uint32_t s_ping_got = 0;
uint32_t s_ping_want_addr = 0;

struct ArpEnt {
    bool valid = false;
    uint32_t ip = 0;  // host order
    uint8_t mac[6]{};
    uint64_t last_ms = 0;
};
constexpr int ARP_N = 16;
ArpEnt s_arp[ARP_N]{};

bool mac_eq(const uint8_t a[6], const uint8_t b[6]) {
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return false;
    return true;
}
bool mac_is_bcast(const uint8_t m[6]) {
    for (int i = 0; i < 6; i++)
        if (m[i] != 0xFF) return false;
    return true;
}

uint32_t subnet_bcast() { return (s_ip & s_mask) | ~s_mask; }
bool is_bcast_dst(uint32_t dst) { return dst == IP_BCAST_ALL || dst == subnet_bcast(); }

ArpEnt *arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_N; i++)
        if (s_arp[i].valid && s_arp[i].ip == ip) return &s_arp[i];
    return nullptr;
}
void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    if (ArpEnt *e = arp_lookup(ip)) {
        util::memcpy(e->mac, mac, 6);
        e->last_ms = internal::now_ms();
        return;
    }
    for (int i = 0; i < ARP_N; i++) {
        if (!s_arp[i].valid) {
            s_arp[i].valid = true;
            s_arp[i].ip = ip;
            util::memcpy(s_arp[i].mac, mac, 6);
            s_arp[i].last_ms = internal::now_ms();
            return;
        }
    }
    int oldest = 0;
    for (int i = 1; i < ARP_N; i++)
        if (s_arp[i].last_ms < s_arp[oldest].last_ms) oldest = i;
    s_arp[oldest].valid = true;
    s_arp[oldest].ip = ip;
    util::memcpy(s_arp[oldest].mac, mac, 6);
    s_arp[oldest].last_ms = internal::now_ms();
}

// Raw Ethernet send. CALLER MUST HOLD internal::Lock (static buffer).
bool send_eth_locked(const uint8_t dst[6], uint16_t ethertype, const void *payload,
                     uint16_t len) {
    if (!s_ready) return false;
    if (len + (uint16_t)sizeof(Eth) > 1514) return false;
    static uint8_t frame[1514];
    Eth *e = (Eth *)frame;
    util::memcpy(e->dst, dst, 6);
    util::memcpy(e->src, s_dev.mac, 6);
    e->type = internal::bswap16(ethertype);
    util::memcpy(frame + sizeof(Eth), payload, len);
    int rc = virtio_net::send_packet(&s_dev, frame, sizeof(Eth) + len);
    if (rc > 0) {
        s_stats.tx++;
        s_stats.tx_bytes += sizeof(Eth) + len;
        return true;
    }
    s_stats.dropped++;
    return false;
}

void arp_request(uint32_t target_ip) {
    ArpPkt a{};
    a.htype = internal::bswap16(1);
    a.ptype = internal::bswap16(0x0800);
    a.hlen = 6;
    a.plen = 4;
    a.op = internal::bswap16(1);
    util::memcpy(a.sha, s_dev.mac, 6);
    internal::ip_to_bytes(s_ip, a.spa);
    internal::ip_to_bytes(target_ip, a.tpa);
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (send_eth_locked(bcast, ETH_ARP, &a, sizeof(a))) s_stats.arp_tx++;
}

void arp_reply(const uint8_t dst_mac[6], uint32_t dst_ip) {
    ArpPkt a{};
    a.htype = internal::bswap16(1);
    a.ptype = internal::bswap16(0x0800);
    a.hlen = 6;
    a.plen = 4;
    a.op = internal::bswap16(2);
    util::memcpy(a.sha, s_dev.mac, 6);
    internal::ip_to_bytes(s_ip, a.spa);
    util::memcpy(a.tha, dst_mac, 6);
    internal::ip_to_bytes(dst_ip, a.tpa);
    if (send_eth_locked(dst_mac, ETH_ARP, &a, sizeof(a))) s_stats.arp_tx++;
}

void handle_arp(const uint8_t src_mac[6], const uint8_t *payload, uint16_t len) {
    s_stats.arp_rx++;
    if (len < sizeof(ArpPkt)) {
        s_stats.dropped++;
        return;
    }
    const ArpPkt *a = (const ArpPkt *)payload;
    if (internal::bswap16(a->htype) != 1 || internal::bswap16(a->ptype) != 0x0800 ||
        a->hlen != 6 || a->plen != 4)
        return;
    uint32_t spa = internal::ip_of(a->spa);
    uint32_t tpa = internal::ip_of(a->tpa);
    arp_learn(spa, a->sha);
    uint16_t op = internal::bswap16(a->op);
    if (op == 1 && tpa == s_ip) {
        // Request for us: reply unicast.
        arp_reply(a->sha, spa);
    }
    (void)src_mac;
}

void icmp_reply(const uint8_t src_mac[6], uint32_t src_ip, const IcmpHdr *req,
                uint16_t icmp_len) {
    if (icmp_len < sizeof(IcmpHdr)) return;
    static uint8_t buf[1500];
    uint16_t total = icmp_len;
    if (total > sizeof(buf)) return;
    util::memcpy(buf, req, icmp_len);
    IcmpHdr *r = (IcmpHdr *)buf;
    r->type = 0;
    r->code = 0;
    r->csum = 0;
    r->csum = internal::bswap16(internal::checksum(buf, total));
    if (send_ipv4(src_ip, IP_ICMP, buf, total)) s_stats.icmp_tx++;
    (void)src_mac;
}

void handle_icmp(const uint8_t src_mac[6], uint32_t src_ip, const uint8_t *payload,
                 uint16_t len) {
    s_stats.icmp_rx++;
    if (len < sizeof(IcmpHdr)) {
        s_stats.dropped++;
        return;
    }
    const IcmpHdr *h = (const IcmpHdr *)payload;
    if (h->type == 8) {
        icmp_reply(src_mac, src_ip, h, len);  // echo request -> reply
    } else if (h->type == 0) {
        if (src_ip == s_ping_want_addr && internal::bswap16(h->id) == s_ping_id) {
            s_ping_got = s_ping_got + 1;
            s_stats.icmp_reply++;
        }
    }
}

void handle_ipv4(const uint8_t src_mac[6], const uint8_t *frame, uint16_t len) {
    s_stats.ip_rx++;
    // Bounds 1: fixed IPv4 header must fit.
    if (len < sizeof(IpHdr)) {
        s_stats.dropped++;
        return;
    }
    const IpHdr *h = (const IpHdr *)frame;
    if ((h->ver_ihl >> 4) != 4) return;
    uint8_t ihl = (h->ver_ihl & 0xF) * 4;
    // Bounds 2: IHL sane and inside the frame.
    if (ihl < 20 || len < ihl) {
        s_stats.dropped++;
        return;
    }
    if (internal::checksum(frame, ihl) != 0) {
        s_stats.dropped++;
        return;
    }
    uint32_t dst = internal::ip_of(h->dst);
    uint32_t src = internal::ip_of(h->src);
    // Bounds 3: total length sane before slicing the payload.
    uint16_t total = internal::bswap16(h->total);
    if (total > len || total < ihl) {
        s_stats.dropped++;
        return;
    }
    const uint8_t *pl = frame + ihl;
    uint16_t pl_len = total - ihl;

    bool for_us = (dst == s_ip) || (dst == IP_BCAST_ALL) || (dst == subnet_bcast());
    if (!for_us) {
        // Exception: while the DHCP client is unbound it must hear offers
        // sent to the not-yet-owned address (or broadcast). DHCP validates
        // xid/chaddr/magic itself; only UDP dport 68 passes here.
        // Bounds 4: need proto + 4 bytes (UDP ports) before peeking dport.
        if (h->proto != IP_UDP || pl_len < 4 || dhcp::bound()) {
            s_stats.dropped++;
            return;
        }
        const UdpHdr *u = (const UdpHdr *)pl;
        if (internal::bswap16(u->dport) != 68) {
            s_stats.dropped++;
            return;
        }
    }
    arp_learn(src, src_mac);  // snoop sender
    if (h->proto == IP_ICMP)
        handle_icmp(src_mac, src, pl, pl_len);
    else if (h->proto == IP_UDP)
        udp::dispatch(src, dst, pl, pl_len);
    else if (h->proto == IP_TCP)
        tcp::dispatch(src, dst, pl, pl_len);
}

void handle_frame(const uint8_t *frame, uint16_t len) {
    s_stats.rx++;
    s_stats.rx_bytes += len;
    if (len < sizeof(Eth)) {
        s_stats.dropped++;
        return;
    }
    const Eth *e = (const Eth *)frame;
    // Accept unicast-to-us, broadcast, or QEMU-multicast prefix 01:.
    if (!mac_eq(e->dst, s_dev.mac) && !mac_is_bcast(e->dst) && e->dst[0] != 0x01) {
        s_stats.dropped++;
        return;
    }
    uint16_t type = internal::bswap16(e->type);
    const uint8_t *pl = frame + sizeof(Eth);
    uint16_t pl_len = len - sizeof(Eth);
    if (type == ETH_ARP)
        handle_arp(e->src, pl, pl_len);
    else if (type == ETH_IP)
        handle_ipv4(e->src, pl, pl_len);
    else
        s_stats.dropped++;
}

// Shared ARP wait (used by ping's prelude and DNS). Runs inside the
// anonymous namespace so it sees arp_lookup/arp_request directly.
bool arp_resolve_impl(uint32_t next_hop, uint32_t timeout_ms) {
    {
        internal::Lock guard;
        if (!arp_lookup(next_hop)) arp_request(next_hop);
    }
    uint64_t t0 = internal::now_ms();
    for (;;) {
        bool have = false;
        {
            internal::Lock guard;
            have = arp_lookup(next_hop) != nullptr;
        }
        if (have || internal::now_ms() - t0 >= timeout_ms) return have;
        // poll() takes its own lock; pause outside all locks. Yield (not a
        // busy spin): when this runs inside a syscall, interrupts are masked
        // on entry, so without a yield neither the PIT (ticks) nor the NIC RX
        // IRQ can fire and the wait would deadlock.
        poll();
        sched::sleep_ms(1);
    }
}

}  // namespace

namespace internal {
bool arp_resolve(uint32_t next_hop, uint32_t timeout_ms) {
    return arp_resolve_impl(next_hop, timeout_ms);
}
}  // namespace internal

// Called by the DHCP thread on ACK. Single writer (thread), lock-free
// single-copy stores; readers use plain loads (eventual consistency).
void apply_dhcp_config(uint32_t new_ip, uint32_t new_mask, uint32_t new_gw, uint32_t new_dns);
void apply_dhcp_config(uint32_t new_ip, uint32_t new_mask, uint32_t new_gw,
                       uint32_t new_dns) {
    s_ip = new_ip;
    s_mask = new_mask;
    s_gw = new_gw;
    s_dns = new_dns;
    s_source = IpSource::DHCP;
}
void apply_fallback_config();
void apply_fallback_config() { s_source = IpSource::FALLBACK; }

void init() {
    if (s_ready) return;
    if (!virtio_net::probe_and_init(&s_dev)) {
        console::kprintf(FG, BG, "[net] no NIC; networking disabled\n");
        return;
    }
    s_ready = true;
    char ips[16], gw[16], mk[16];
    fmt_ipv4(s_ip, ips);
    fmt_ipv4(s_gw, gw);
    fmt_ipv4(s_mask, mk);
    console::kprintf(OK, BG, "[net] up: ip %s mask %s gw %s (default, dhcp soon)\n", ips, mk,
                     gw);
    poll();
}

void poll() {
    if (!s_ready) return;
    internal::Lock guard;
    // NOTE: no MMIO refresh here (suspect #1 for link-down stalls; the
    // send path re-reads link live when it matters).
    static uint8_t buf[2048];
    for (int i = 0; i < 32; i++) {
        int n = virtio_net::recv_packet(&s_dev, buf, sizeof(buf));
        if (n <= 0) break;
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        handle_frame(buf, (uint16_t)n);
    }
    tcp::poll_timers();
}

bool ready() { return s_ready; }
bool link_up() { return s_ready && s_dev.link_up; }
const uint8_t *mac() { return s_ready ? s_dev.mac : nullptr; }
uint32_t ip() { return s_ip; }
uint32_t gw() { return s_gw; }
uint32_t mask() { return s_mask; }
uint32_t dns_server() { return s_dns; }
// The name is ours to choose (one NIC, no discovery), but it is only reported
// when a NIC was actually claimed.
const char *if_name() { return s_ready ? "eth0" : ""; }
const char *if_driver() { return s_ready ? "virtio-net" : ""; }
uint16_t if_mtu() { return s_ready ? virtio_net::get_mtu(&s_dev) : 0; }
IpSource ip_source() { return s_source; }
const char *ip_source_str() {
    switch (s_source) {
        case IpSource::DHCP:
            return "dhcp";
        case IpSource::FALLBACK:
            return "fallback";
        default:
            return "default";
    }
}

bool send_ipv4_from(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const void *payload,
                    uint16_t len) {
    if (!s_ready || !payload) return false;
    if (len + sizeof(IpHdr) > 1500) return false;
    internal::Lock guard;
    const uint8_t *dst_mac = nullptr;
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ArpEnt *e = nullptr;
    if (is_bcast_dst(dst_ip)) {
        dst_mac = bcast;  // broadcast skips ARP entirely
    } else {
        e = arp_lookup(dst_ip);
        if (!e) {
            uint32_t next = dst_ip;
            // Off-subnet goes via the gateway.
            if ((dst_ip & s_mask) != (s_ip & s_mask)) next = s_gw;
            // Never ARP for 0.0.0.0 (unbound DHCP has no usable source).
            if (next == 0) return false;
            arp_request(next);
            return false;
        }
        dst_mac = e->mac;
    }
    static uint8_t pkt[1500];
    IpHdr *h = (IpHdr *)pkt;
    h->ver_ihl = 0x45;
    h->tos = 0;
    h->total = internal::bswap16(sizeof(IpHdr) + len);
    h->id = internal::bswap16(s_ip_id++);
    h->frag = internal::bswap16(0x4000);  // don't fragment
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    internal::ip_to_bytes(src_ip, h->src);
    internal::ip_to_bytes(dst_ip, h->dst);
    util::memcpy(pkt + sizeof(IpHdr), payload, len);
    h->csum = internal::bswap16(internal::checksum(pkt, sizeof(IpHdr)));
    s_stats.ip_tx++;
    return send_eth_locked(dst_mac, ETH_IP, pkt, sizeof(IpHdr) + len);
}

bool send_ipv4(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len) {
    return send_ipv4_from(s_ip, dst_ip, proto, payload, len);
}

int ping(uint32_t dst_ip, int count, uint32_t timeout_ms) {
    if (!s_ready || count <= 0) return 0;
    // Ensure ARP resolved first (gateway or target). Wait up to 1s.
    // Locking: lookups/requests under the lock, poll() takes its own lock.
    uint32_t next;
    {
        internal::Lock guard;
        next = ((dst_ip & s_mask) == (s_ip & s_mask)) ? dst_ip : s_gw;
    }
    internal::arp_resolve(next, 1000);
    int got = 0;
    s_ping_id++;
    for (int i = 0; i < count; i++) {
        s_ping_seq++;
        s_ping_got = 0;
        s_ping_want_addr = dst_ip;
        uint8_t req[sizeof(IcmpHdr) + 32];
        IcmpHdr *h = (IcmpHdr *)req;
        h->type = 8;
        h->code = 0;
        h->csum = 0;
        h->id = internal::bswap16(s_ping_id);
        h->seq = internal::bswap16(s_ping_seq);
        for (int k = 0; k < 32; k++) req[sizeof(IcmpHdr) + k] = (uint8_t)(0xA5 + k);
        h->csum = internal::bswap16(internal::checksum(req, sizeof(req)));
        uint32_t before = s_stats.icmp_reply;
        if (send_ipv4(dst_ip, IP_ICMP, req, sizeof(req))) s_stats.icmp_tx++;
        uint64_t t0 = internal::now_ms();
        while (internal::now_ms() - t0 < timeout_ms) {
            poll();
            if (s_ping_got > 0) break;
            for (int k = 0; k < 20000; k++) asm volatile("pause");
        }
        if (s_stats.icmp_reply != before || s_ping_got > 0) {
            got++;
            console::kprintf(OK, BG, "[ping] reply seq=%u\n", s_ping_seq);
        } else {
            console::kprintf(FG, BG, "[ping] timeout seq=%u\n", s_ping_seq);
        }
        uint64_t g0 = internal::now_ms();
        while (internal::now_ms() - g0 < 200) poll();
    }
    s_ping_want_addr = 0;
    return got;
}

bool parse_ipv4(const char *s, uint32_t &out) {
    uint32_t parts[4] = {};
    int idx = 0;
    uint32_t cur = 0;
    bool digit = false;
    for (const char *p = s;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (uint32_t)(c - '0');
            if (cur > 255) return false;
            digit = true;
        } else if (c == '.' || c == 0) {
            if (!digit || idx >= 4) return false;
            parts[idx++] = cur;
            cur = 0;
            digit = false;
            if (c == 0) break;
        } else {
            return false;
        }
    }
    if (idx != 4) return false;
    out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return true;
}

void fmt_ipv4(uint32_t ip, char *out) {
    uint8_t b[4];
    internal::ip_to_bytes(ip, b);
    char *p = out;
    for (int i = 0; i < 4; i++) {
        char tmp[4];
        util::to_str(b[i], tmp);
        for (char *q = tmp; *q; q++) *p++ = *q;
        if (i < 3) *p++ = '.';
    }
    *p = 0;
}

void fmt_mac(const uint8_t *m, char *out) {
    static const char *hex = "0123456789abcdef";
    char *p = out;
    for (int i = 0; i < 6; i++) {
        *p++ = hex[(m[i] >> 4) & 0xF];
        *p++ = hex[m[i] & 0xF];
        if (i < 5) *p++ = ':';
    }
    *p = 0;
}

const Stats &stats() { return s_stats; }
int arp_count() {
    int n = 0;
    for (int i = 0; i < ARP_N; i++)
        if (s_arp[i].valid) n++;
    return n;
}
bool arp_get(int idx, uint32_t *ip, uint8_t mac_out[6]) {
    int n = 0;
    for (int i = 0; i < ARP_N; i++) {
        if (!s_arp[i].valid) continue;
        if (n == idx) {
            if (ip) *ip = s_arp[i].ip;
            if (mac_out) util::memcpy(mac_out, s_arp[i].mac, 6);
            return true;
        }
        n++;
    }
    return false;
}
bool arp_at(int idx, uint32_t *ip, uint8_t mac_out[6], uint64_t *last_ms) {
    internal::Lock guard;
    int n = 0;
    for (int i = 0; i < ARP_N; i++) {
        if (!s_arp[i].valid) continue;
        if (n == idx) {
            if (ip) *ip = s_arp[i].ip;
            if (mac_out) util::memcpy(mac_out, s_arp[i].mac, 6);
            if (last_ms) *last_ms = s_arp[i].last_ms;
            return true;
        }
        n++;
    }
    return false;
}
void arp_flush() {
    internal::Lock guard;
    for (int i = 0; i < ARP_N; i++) s_arp[i].valid = false;
}

}  // namespace choco::net

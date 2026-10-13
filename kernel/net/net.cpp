// ChocoOS net stack, passo 1: Ethernet + ARP + IPv4 + ICMP.
// Static QEMU user-net config, polled RX (no IRQs yet).
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/net.hpp>
#include <choco/paging.hpp>
#include <choco/serial.hpp>
#include <choco/util.hpp>
#include <choco/virtio_net.hpp>

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;

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
#pragma pack(pop)

constexpr uint16_t ETH_ARP = 0x0806;
constexpr uint16_t ETH_IP = 0x0800;
constexpr uint8_t IP_ICMP = 1;

virtio_net::NetDevice s_dev{};
bool s_ready = false;
Stats s_stats{};
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

uint64_t now_ms() { return pit::ticks() * 10; }  // HZ=100

inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }
inline uint32_t ip_of(const uint8_t b[4]) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
inline void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = ip & 0xFF;
    out[1] = (ip >> 8) & 0xFF;
    out[2] = (ip >> 16) & 0xFF;
    out[3] = (ip >> 24) & 0xFF;
}

uint16_t checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

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

ArpEnt *arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_N; i++)
        if (s_arp[i].valid && s_arp[i].ip == ip) return &s_arp[i];
    return nullptr;
}
void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    if (ArpEnt *e = arp_lookup(ip)) {
        util::memcpy(e->mac, mac, 6);
        e->last_ms = now_ms();
        return;
    }
    for (int i = 0; i < ARP_N; i++) {
        if (!s_arp[i].valid) {
            s_arp[i].valid = true;
            s_arp[i].ip = ip;
            util::memcpy(s_arp[i].mac, mac, 6);
            s_arp[i].last_ms = now_ms();
            return;
        }
    }
    // Evict oldest
    int oldest = 0;
    for (int i = 1; i < ARP_N; i++)
        if (s_arp[i].last_ms < s_arp[oldest].last_ms) oldest = i;
    s_arp[oldest].valid = true;
    s_arp[oldest].ip = ip;
    util::memcpy(s_arp[oldest].mac, mac, 6);
    s_arp[oldest].last_ms = now_ms();
}

bool send_eth(const uint8_t dst[6], uint16_t ethertype, const void *payload, uint16_t len) {
    if (!s_ready) return false;
    if (len + (uint16_t)sizeof(Eth) > 1514) return false;
    static uint8_t frame[1514];
    Eth *e = (Eth *)frame;
    util::memcpy(e->dst, dst, 6);
    util::memcpy(e->src, s_dev.mac, 6);
    e->type = bswap16(ethertype);
    util::memcpy(frame + sizeof(Eth), payload, len);
    int rc = virtio_net::send_packet(&s_dev, frame, sizeof(Eth) + len);
    if (rc > 0) {
        s_stats.tx++;
        return true;
    }
    s_stats.dropped++;
    return false;
}

void arp_request(uint32_t target_ip) {
    ArpPkt a{};
    a.htype = bswap16(1);
    a.ptype = bswap16(0x0800);
    a.hlen = 6;
    a.plen = 4;
    a.op = bswap16(1);
    util::memcpy(a.sha, s_dev.mac, 6);
    ip_to_bytes(IP_GUEST, a.spa);
    ip_to_bytes(target_ip, a.tpa);
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (send_eth(bcast, ETH_ARP, &a, sizeof(a))) s_stats.arp_tx++;
}

void arp_reply(const uint8_t dst_mac[6], uint32_t dst_ip) {
    ArpPkt a{};
    a.htype = bswap16(1);
    a.ptype = bswap16(0x0800);
    a.hlen = 6;
    a.plen = 4;
    a.op = bswap16(2);
    util::memcpy(a.sha, s_dev.mac, 6);
    ip_to_bytes(IP_GUEST, a.spa);
    util::memcpy(a.tha, dst_mac, 6);
    ip_to_bytes(dst_ip, a.tpa);
    if (send_eth(dst_mac, ETH_ARP, &a, sizeof(a))) s_stats.arp_tx++;
}

void handle_arp(const uint8_t src_mac[6], const uint8_t *payload, uint16_t len) {
    s_stats.arp_rx++;
    if (len < sizeof(ArpPkt)) {
        s_stats.dropped++;
        return;
    }
    const ArpPkt *a = (const ArpPkt *)payload;
    if (bswap16(a->htype) != 1 || bswap16(a->ptype) != 0x0800 || a->hlen != 6 || a->plen != 4)
        return;
    uint32_t spa = ip_of(a->spa);
    uint32_t tpa = ip_of(a->tpa);
    arp_learn(spa, a->sha);
    uint16_t op = bswap16(a->op);
    if (op == 1 && tpa == IP_GUEST) {
        // Request for us: reply unicast.
        arp_reply(a->sha, spa);
    }
    (void)src_mac;
}

void icmp_reply(const uint8_t src_mac[6], uint32_t src_ip, const IcmpHdr *req, uint16_t icmp_len) {
    if (icmp_len < sizeof(IcmpHdr)) return;
    static uint8_t buf[1500];
    uint16_t total = icmp_len;
    if (total > sizeof(buf)) return;
    util::memcpy(buf, req, icmp_len);
    IcmpHdr *r = (IcmpHdr *)buf;
    r->type = 0;
    r->code = 0;
    r->csum = 0;
    // csum over header+data, stored big-endian
    uint16_t c = checksum(buf, total);
    r->csum = bswap16(c);
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
        // Echo reply: check pending ping (id match, seq any).
        if (src_ip == s_ping_want_addr && bswap16(h->id) == s_ping_id) {
            s_ping_got = s_ping_got + 1;
            s_stats.icmp_reply++;
        }
    }
}

void handle_ipv4(const uint8_t src_mac[6], const uint8_t *frame, uint16_t len) {
    s_stats.ip_rx++;
    if (len < sizeof(IpHdr)) {
        s_stats.dropped++;
        return;
    }
    const IpHdr *h = (const IpHdr *)frame;
    if ((h->ver_ihl >> 4) != 4) return;
    uint8_t ihl = (h->ver_ihl & 0xF) * 4;
    if (ihl < 20 || len < ihl) return;
    // Verify checksum
    IpHdr tmp{};
    util::memcpy(&tmp, h, sizeof(tmp));
    // checksum field is big-endian on wire; checksum() expects network order
    // bytes as-is, so compute over the received bytes directly.
    if (checksum(frame, ihl) != 0) {
        s_stats.dropped++;
        return;
    }
    uint32_t dst = ip_of(h->dst);
    if (dst != IP_GUEST && dst != IP_BCAST) return;  // not for us
    uint32_t src = ip_of(h->src);
    arp_learn(src, src_mac);  // snoop sender
    uint16_t total = bswap16(h->total);
    if (total > len || total < ihl) return;
    const uint8_t *pl = frame + ihl;
    uint16_t pl_len = total - ihl;
    if (h->proto == IP_ICMP) handle_icmp(src_mac, src, pl, pl_len);
    // TCP/UDP arrive in later phases: count and drop for now.
}

void handle_frame(const uint8_t *frame, uint16_t len) {
    s_stats.rx++;
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
    uint16_t type = bswap16(e->type);
    const uint8_t *pl = frame + sizeof(Eth);
    uint16_t pl_len = len - sizeof(Eth);
    if (type == ETH_ARP)
        handle_arp(e->src, pl, pl_len);
    else if (type == ETH_IP)
        handle_ipv4(e->src, pl, pl_len);
    else
        s_stats.dropped++;
}

}  // namespace

void init() {
    if (s_ready) return;
    if (!virtio_net::probe_and_init(&s_dev)) {
        console::kprintf(FG, BG, "[net] no NIC; networking disabled\n");
        return;
    }
    s_ready = true;
    console::kprintf(OK, BG, "[net] up: ip 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2 dns 10.0.2.3\n");
    // Announce ourselves so the gateway learns our MAC fast (helps pcap).
    arp_request(IP_GW);
    poll();
}

void poll() {
    if (!s_ready) return;
    static uint8_t buf[2048];
    for (int i = 0; i < 32; i++) {
        int n = virtio_net::recv_packet(&s_dev, buf, sizeof(buf));
        if (n <= 0) break;
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        handle_frame(buf, (uint16_t)n);
    }
}

bool ready() { return s_ready; }
bool link_up() { return s_ready && s_dev.link_up; }
const uint8_t *mac() { return s_ready ? s_dev.mac : nullptr; }
uint32_t ip() { return IP_GUEST; }
uint32_t gw() { return IP_GW; }
uint32_t mask() { return IP_MASK; }
uint32_t dns() { return IP_DNS; }

bool send_ipv4(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len) {
    if (!s_ready) return false;
    if (len + sizeof(IpHdr) > 1500) return false;
    ArpEnt *e = arp_lookup(dst_ip);
    // Same-subnet miss or gateway miss: resolve first, drop this packet.
    if (!e) {
        uint32_t next = dst_ip;
        // QEMU user-net: everything off-subnet goes via the gateway.
        if ((dst_ip & IP_MASK) != (IP_GUEST & IP_MASK)) next = IP_GW;
        arp_request(next);
        return false;
    }
    static uint8_t pkt[1500];
    IpHdr *h = (IpHdr *)pkt;
    h->ver_ihl = 0x45;
    h->tos = 0;
    h->total = bswap16(sizeof(IpHdr) + len);
    h->id = bswap16(s_ip_id++);
    h->frag = bswap16(0x4000);  // don't fragment
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    ip_to_bytes(IP_GUEST, h->src);
    ip_to_bytes(dst_ip, h->dst);
    util::memcpy(pkt + sizeof(IpHdr), payload, len);
    h->csum = bswap16(checksum(pkt, sizeof(IpHdr)));
    s_stats.ip_tx++;
    return send_eth(e->mac, ETH_IP, pkt, sizeof(IpHdr) + len);
}

int ping(uint32_t dst_ip, int count, uint32_t timeout_ms) {
    if (!s_ready || count <= 0) return 0;
    // Ensure ARP resolved first (gateway or target). Wait up to 1s.
    uint32_t next = ((dst_ip & IP_MASK) == (IP_GUEST & IP_MASK)) ? dst_ip : IP_GW;
    if (!arp_lookup(next)) {
        arp_request(next);
        uint64_t t0 = now_ms();
        while (now_ms() - t0 < 1000) {
            poll();
            if (arp_lookup(next)) break;
            for (int i = 0; i < 20000; i++) asm volatile("pause");
        }
    }
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
        h->id = bswap16(s_ping_id);
        h->seq = bswap16(s_ping_seq);
        for (int k = 0; k < 32; k++) req[sizeof(IcmpHdr) + k] = (uint8_t)(0xA5 + k);
        h->csum = bswap16(checksum(req, sizeof(req)));
        uint32_t before = s_stats.icmp_reply;
        if (send_ipv4(dst_ip, IP_ICMP, req, sizeof(req))) s_stats.icmp_tx++;
        uint64_t t0 = now_ms();
        while (now_ms() - t0 < timeout_ms) {
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
        // Gap between probes
        uint64_t g0 = now_ms();
        while (now_ms() - g0 < 200) poll();
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
    ip_to_bytes(ip, b);
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

}  // namespace choco::net

// ChocoOS DHCP client (RFC 2131) — background thread, bounded retries.
//
// DESIGN (requirements):
//  - Never blocks boot: dhcp::start() spawns a thread and returns at once;
//    the whole handshake runs concurrently with the shell. Worst case the
//    thread exits after ~16s and applies the compiled-in fallback.
//  - Retries: up to 4 DISCOVERs then up to 4 REQUESTs, 2s each.
//  - Fallback keeps the QEMU user-net defaults (10.0.2.15/24), so a missing
//    server degrades to net-1 behavior instead of hanging.
//
// BOUNDS AUDIT (untrusted input: every inbound byte is length-checked):
//  - (D1) datagram must hold BOOTP fixed part (236B) + magic (4B).
//  - (D2) op/htype/hlen checked before touching chaddr.
//  - (D3) xid + chaddr + magic must match our discover (anti-spoof).
//  - (D4) options walk never reads past the datagram: each step checks the
//    tag byte, the length byte, and len bytes against the remaining count;
//    malformed options abort the walk (packet dropped).
//  - (D5) multi-byte fields assembled byte-wise (no unaligned u32 loads).
//  - (D6) option 53/54/1/3/6/51 values length-checked (1/4/4/4+/4+/4).
// Outbound packets are built into a fixed 300B stack buffer, sizes static.
#include <choco/console.hpp>
#include <choco/net.hpp>
#include <choco/sched.hpp>
#include <choco/util.hpp>

#include "net_internal.hpp"

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;
namespace internal = choco::net::internal;

namespace choco::net::dhcp {
namespace {

constexpr uint16_t CLIENT_PORT = 68;
constexpr uint16_t SERVER_PORT = 67;
constexpr uint32_t BCAST_IP = 0xFFFFFFFFu;
constexpr size_t BOOTP_FIXED = 236;
constexpr size_t DHCP_MIN = 300;
constexpr int MAX_DISCOVER = 4;
constexpr int MAX_REQUEST = 4;
constexpr uint32_t WAIT_MS = 2000;

enum class State : uint8_t { IDLE, DISCOVERING, REQUESTING, BOUND, FALLBACK };
volatile State s_state = State::IDLE;
volatile bool s_running = false;
uint32_t s_server = 0;
uint32_t s_lease = 0;
uint32_t s_xid_counter = 0;

struct Offer {
    bool got = false;
    uint32_t yiaddr = 0;
    uint32_t server = 0;
    uint32_t mask = 0;
    uint32_t gw = 0;
    uint32_t dns = 0;
    uint32_t lease = 0;
};

uint32_t make_xid() {
    const uint8_t *m = mac();
    uint32_t x = (uint32_t)(pit::ticks() & 0xFFFFFFFFu);
    if (m)
        x ^= ((uint32_t)m[3] << 24) | ((uint32_t)m[4] << 16) | ((uint32_t)m[5] << 8) | 0xD9C1u;
    x ^= (++s_xid_counter * 0x9E3779B1u);
    return x ? x : 0x12345678u;
}

void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Build a 300B DHCP packet. kind: 1=discover, 3=request.
uint16_t build_msg(uint8_t *out, uint8_t kind, uint32_t xid, uint32_t req_ip) {
    for (size_t i = 0; i < DHCP_MIN; i++) out[i] = 0;
    out[0] = 1;  // BOOTREQUEST
    out[1] = 1;  // ethernet
    out[2] = 6;
    out[3] = 0;
    put32be(out + 4, xid);
    out[10] = 0x80;  // flags: broadcast replies (we take bcast or unicast)
    out[11] = 0x00;
    const uint8_t *m = mac();
    if (m)
        for (int i = 0; i < 6; i++) out[28 + i] = m[i];
    out[236] = 99;
    out[237] = 130;
    out[238] = 83;
    out[239] = 99;
    size_t o = 240;
    out[o++] = 53;
    out[o++] = 1;
    out[o++] = kind;
    out[o++] = 61;  // client identifier: type 1 (ethernet) + MAC
    out[o++] = 7;
    out[o++] = 1;
    if (m)
        for (int i = 0; i < 6; i++) out[o++] = m[i];
    else
        o += 6;
    if (kind == 3) {  // requested IP address
        out[o++] = 50;
        out[o++] = 4;
        uint8_t b[4];
        internal::ip_to_bytes(req_ip, b);
        for (int i = 0; i < 4; i++) out[o++] = b[i];
    }
    out[o++] = 55;  // parameter request list
    out[o++] = 5;
    out[o++] = 1;   // subnet mask
    out[o++] = 3;   // router
    out[o++] = 6;   // dns
    out[o++] = 51;  // lease time
    out[o++] = 54;  // server identifier
    out[o++] = 255;
    return DHCP_MIN;
}

uint32_t get32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Parse one server message. want_type: 2=offer, 5=ack (6=nak handled too).
// Returns true on fully validated match; fills Offer. Strict bounds (D4-D6).
bool parse_msg(const uint8_t *pkt, uint16_t len, uint32_t xid, uint8_t want_type, Offer &offer,
               bool &saw_nak) {
    saw_nak = false;
    if (len < BOOTP_FIXED + 4) return false;  // (D1)
    if (pkt[0] != 2) return false;            // must be BOOTREPLY
    if (pkt[1] != 1 || pkt[2] != 6) return false;  // (D2) eth/6
    if (get32be(pkt + 4) != xid) return false;     // (D3) xid
    const uint8_t *m = mac();
    if (!m) return false;
    for (int i = 0; i < 6; i++)
        if (pkt[28 + i] != m[i]) return false;  // (D3) chaddr
    if (pkt[236] != 99 || pkt[237] != 130 || pkt[238] != 83 || pkt[239] != 99)
        return false;  // (D3) magic
    uint32_t yiaddr = internal::ip_of(pkt + 16);
    if (yiaddr == 0) return false;

    uint8_t msg_type = 0;
    uint32_t server = 0, mask = 0, gw = 0, dns = 0, lease = 0;
    size_t p = 240;
    while (p < len) {  // (D4) options walk, always bounded
        uint8_t tag = pkt[p++];
        if (tag == 0) continue;    // pad
        if (tag == 255) break;     // end
        if (p >= len) return false;  // no length byte
        uint8_t olen = pkt[p++];
        if ((size_t)olen > len - p) return false;  // value overruns datagram
        const uint8_t *v = pkt + p;
        switch (tag) {
            case 53:  // (D6) message type, must be 1 byte
                if (olen == 1) msg_type = v[0];
                break;
            case 54:  // server identifier, 4 bytes
                if (olen == 4) server = internal::ip_of(v);
                break;
            case 1:  // subnet mask, 4 bytes
                if (olen == 4) mask = internal::ip_of(v);
                break;
            case 3:  // router(s), take the first 4 bytes
                if (olen >= 4) gw = internal::ip_of(v);
                break;
            case 6:  // dns server(s), take the first 4 bytes
                if (olen >= 4) dns = internal::ip_of(v);
                break;
            case 51:  // lease time, 4 bytes BE seconds
                if (olen == 4) lease = get32be(v);
                break;
            default:
                break;  // ignore unknown options
        }
        p += olen;
    }
    if (msg_type == 6) {
        saw_nak = true;
        return false;
    }
    if (msg_type != want_type) return false;
    offer.got = true;
    offer.yiaddr = yiaddr;
    offer.server = server;
    offer.mask = mask ? mask : (255u | (255u << 8) | (255u << 16) | (0u << 24));
    offer.gw = gw;
    offer.dns = dns ? dns : offer.gw;
    offer.lease = lease;
    return true;
}

// Wait up to timeout for a validated message of want_type.
bool wait_for(uint32_t xid, uint8_t want_type, Offer &offer, uint32_t timeout_ms) {
    static uint8_t buf[1024];
    uint64_t t0 = internal::now_ms();
    while (internal::now_ms() - t0 < timeout_ms) {
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = udp::recv_from(CLIENT_PORT, buf, sizeof(buf), 250, &from_ip, &from_port);
        if (n < 0) continue;  // slice timeout: re-check the deadline
        if (from_port != SERVER_PORT) continue;
        bool nak = false;
        if (parse_msg(buf, (uint16_t)n, xid, want_type, offer, nak)) return true;
        if (nak) return false;  // NAK ends the wait early (caller restarts)
        (void)from_ip;
    }
    return false;
}

void run_once() {
    if (!udp::bind(CLIENT_PORT)) {
        console::println("[dhcp] port 68 busy, skipping", FG);
        return;
    }
    s_running = true;
    s_state = State::DISCOVERING;
    console::println("[dhcp] discovering (broadcast, 4 tries)...", FG);

    uint32_t xid = make_xid();
    Offer offer{};
    bool have_offer = false;
    for (int t = 0; t < MAX_DISCOVER && !have_offer; t++) {
        static uint8_t pkt[DHCP_MIN];
        build_msg(pkt, 1, xid, 0);
        char xids[12];
        util::to_str(xid, xids);
        console::kprintf(FG, BG, "[dhcp] discover try %d xid %s\n", t + 1, xids);
        udp::send_to(0, CLIENT_PORT, BCAST_IP, SERVER_PORT, pkt, sizeof(pkt));
        have_offer = wait_for(xid, 2, offer, WAIT_MS);
    }
    if (!have_offer) {
        console::println("[dhcp] no offer: keeping fallback config", FG);
        apply_fallback_config();
        s_state = State::FALLBACK;
        udp::unbind(CLIENT_PORT);
        s_running = false;
        return;
    }
    char oip[16], sip[16];
    fmt_ipv4(offer.yiaddr, oip);
    fmt_ipv4(offer.server, sip);
    console::kprintf(OK, BG, "[dhcp] offer %s from %s\n", oip, sip);

    s_state = State::REQUESTING;
    bool acked = false;
    for (int r = 0; r < MAX_REQUEST && !acked; r++) {
        static uint8_t pkt[DHCP_MIN];
        build_msg(pkt, 3, xid, offer.yiaddr);
        console::kprintf(FG, BG, "[dhcp] request try %d for %s\n", r + 1, oip);
        udp::send_to(0, CLIENT_PORT, BCAST_IP, SERVER_PORT, pkt, sizeof(pkt));
        Offer ans{};
        acked = wait_for(xid, 5, ans, WAIT_MS);
        if (acked) offer = ans;
    }
    if (!acked) {
        console::println("[dhcp] no ack: keeping fallback config", FG);
        apply_fallback_config();
        s_state = State::FALLBACK;
        udp::unbind(CLIENT_PORT);
        s_running = false;
        return;
    }
    apply_dhcp_config(offer.yiaddr, offer.mask, offer.gw, offer.dns);
    s_server = offer.server;
    s_lease = offer.lease;
    char ips[16], mks[16], gws[16], dns[16];
    fmt_ipv4(offer.yiaddr, ips);
    fmt_ipv4(offer.mask, mks);
    fmt_ipv4(offer.gw, gws);
    fmt_ipv4(offer.dns, dns);
    console::kprintf(OK, BG, "[dhcp] bound %s mask %s gw %s dns %s lease %u s\n", ips, mks,
                     gws, dns, offer.lease);
    s_state = State::BOUND;
    udp::unbind(CLIENT_PORT);
    s_running = false;
}

void dhcp_main(void *) { run_once(); }

}  // namespace

void start() {
    if (s_running) return;
    if (!ready()) return;
    if (!sched::spawn("dhcp", dhcp_main, nullptr))
        console::println("[dhcp] spawn failed", FG);
}

bool running() { return s_running; }
bool bound() { return s_state == State::BOUND; }
const char *state_str() {
    switch (s_state) {
        case State::DISCOVERING:
            return "discovering";
        case State::REQUESTING:
            return "requesting";
        case State::BOUND:
            return "bound";
        case State::FALLBACK:
            return "fallback";
        default:
            return "idle";
    }
}
uint32_t server() { return s_server; }
uint32_t lease_secs() { return s_lease; }
bool renew() {
    if (s_running) return false;
    s_state = State::IDLE;
    if (!sched::spawn("dhcp", dhcp_main, nullptr)) return false;
    return true;
}

}  // namespace choco::net::dhcp

// ChocoOS TCP client (RFC 9293 subset, active-open only) — net-4.
//
// No listen/accept, no socket syscalls here (those come in net-5 on top of
// this engine; the TTY/syscall worker owns that layer — DO NOT add syscalls
// in kernel/net/). Shell test command `httpget` drives this API directly.
//
// Covered: 3-way handshake, in-order data with immediate ACK, go-back-N
// retransmission with backoff RTO, advertised window, graceful FIN exchange,
// RST abort. Deliberately deferred: out-of-order reassembly (dropped, sender
// retransmits), TIME_WAIT (closed immediately, ephemeral ports rotate),
// zero-window persist (clamped to 1 segment), SACK/timestamps (parsed past).
//
// BOUNDS AUDIT (untrusted input):
//  - (F1) segment must hold the 20-byte fixed header.
//  - (F2) data offset 5..15 and header fits the segment.
//  - (F3) checksum verified over pseudo-header + segment (drop on mismatch).
//  - (F4) options walked with kind/len checks; only MSS (kind 2, len 4) read.
//  - (F5) payload length derived from validated header, never negative.
//  - (F6) RX append capped by free buffer space (drop overflow).
//  - (F7) TX append capped by buffer size (partial queue, never overflow).
//  - (F8) answer-RR style caps: RTO retries bounded (abort), waits bounded.
// Locking: public API takes internal::Lock; dispatch() runs under net::poll.
#include <choco/console.hpp>
#include <choco/net.hpp>
#include <choco/sched.hpp>
#include <choco/util.hpp>

#include "net_internal.hpp"

using choco::console::BG;
using choco::console::FG;
namespace internal = choco::net::internal;

namespace choco::net::tcp {
namespace {

#pragma pack(push, 1)
struct Hdr {
    uint16_t sport;  // big-endian
    uint16_t dport;  // big-endian
    uint32_t seq;    // big-endian
    uint32_t ack;    // big-endian
    uint8_t doff;    // high nibble = 32-bit words
    uint8_t flags;
    uint16_t wnd;  // big-endian
    uint16_t csum;  // big-endian
    uint16_t urg;   // big-endian
};
#pragma pack(pop)

constexpr uint8_t F_FIN = 0x01;
constexpr uint8_t F_SYN = 0x02;
constexpr uint8_t F_RST = 0x04;
constexpr uint8_t F_PSH = 0x08;
constexpr uint8_t F_ACK = 0x10;

constexpr uint16_t OUR_MSS = 1460;
constexpr uint16_t RX_BUF = 16384;
constexpr uint16_t TX_BUF = 16384;
constexpr int MAX_FLIGHT_SEGS = 4;
constexpr uint32_t RTO_INIT = 1000;
constexpr uint32_t RTO_MAX = 8000;
constexpr int MAX_RETRIES = 5;

enum class St : uint8_t { FREE, LISTEN, SYN_SENT, SYN_RCVD, EST, FIN_W1, FIN_W2, CLOSE_WAIT, LAST_ACK, CLOSED };

struct Tcb {
    bool used = false;
    St st = St::FREE;
    int err = 0;  // 0 ok, -1 timeout/refused/RST
    uint32_t lip = 0, rip = 0;
    uint16_t lport = 0, rport = 0;
    uint32_t snd_una = 0, snd_nxt = 0, snd_wnd = 0;
    uint16_t peer_mss = OUR_MSS;
    uint32_t rcv_nxt = 0;
    uint8_t rx[RX_BUF]{};
    uint16_t rx_len = 0;
    uint8_t tx[TX_BUF]{};
    uint16_t tx_len = 0;  // bytes from snd_una onward (acked prefix removed)
    uint32_t rto_ms = RTO_INIT;
    uint64_t rto_deadline = 0;  // 0 = disarmed
    int retries = 0;
    bool fin_sent = false, fin_acked = false, fin_rcvd = false;
    int listener = -1;  // parent listener handle (inbound children), -1 otherwise
    int backlog = 0;    // LISTEN only: max half-open+pending children (clamped)
};
Tcb s_tcb[MAX_CONN]{};
ConnStats s_stats{};
uint16_t s_next_port = 40000;
uint32_t s_isn = 0x1640CA71u;

inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

inline void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
inline uint32_t get32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
// Shift a prefix out of a stream buffer (always dest < src: forward copy safe).
inline void drop_prefix(uint8_t *buf, uint16_t from, uint16_t total) {
    for (uint16_t i = from; i < total; i++) buf[i - from] = buf[i];
}

Tcb *by_handle(Handle h) {
    if (h < 0 || h >= MAX_CONN) return nullptr;
    if (!s_tcb[h].used) return nullptr;
    return &s_tcb[h];
}
Tcb *by_quad(uint32_t rip, uint16_t rport, uint16_t lport) {
    for (int i = 0; i < MAX_CONN; i++) {
        Tcb *t = &s_tcb[i];
        if (!t->used || t->st == St::FREE || t->st == St::CLOSED) continue;
        if (t->rip == rip && t->rport == rport && t->lport == lport) return t;
    }
    return nullptr;
}

uint16_t seg_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len) {
    uint8_t pseudo[12];
    internal::ip_to_bytes(src, pseudo);
    internal::ip_to_bytes(dst, pseudo + 4);
    pseudo[8] = 0;
    pseudo[9] = 6;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)(len & 0xFF);
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += ((uint32_t)pseudo[i] << 8) | pseudo[i + 1];
    for (uint16_t i = 0; i + 1 < len; i += 2) sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (len & 1) sum += (uint32_t)seg[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return c == 0 ? 0xFFFF : c;
}

// Raw segment send. CALLER HOLDS internal::Lock.
bool raw_send(Tcb *t, uint8_t flags, uint32_t seq, const void *data, uint16_t dlen,
              bool with_mss) {
    static uint8_t seg[1500];
    uint16_t hlen = with_mss ? 24 : 20;
    if ((uint32_t)hlen + dlen > sizeof(seg)) return false;
    Hdr *h = (Hdr *)seg;
    h->sport = internal::bswap16(t->lport);
    h->dport = internal::bswap16(t->rport);
    put32be((uint8_t *)&h->seq, seq);
    put32be((uint8_t *)&h->ack, t->rcv_nxt);
    h->doff = (uint8_t)(hlen / 4) << 4;
    h->flags = flags;
    uint16_t rx_free = RX_BUF - t->rx_len;
    h->wnd = internal::bswap16(rx_free);
    h->csum = 0;
    h->urg = 0;
    if (with_mss) {
        seg[20] = 2;
        seg[21] = 4;
        seg[22] = (uint8_t)(OUR_MSS >> 8);
        seg[23] = (uint8_t)OUR_MSS;
    }
    if (dlen) util::memcpy(seg + hlen, data, dlen);
    // checksum over pseudo-header + segment (csum field zeroed)
    uint16_t c = seg_checksum(t->lip, t->rip, seg, (uint16_t)(hlen + dlen));
    h->csum = internal::bswap16(c);
    if (!send_ipv4_from(t->lip, t->rip, 6, seg, (uint16_t)(hlen + dlen))) return false;
    s_stats.tx_segs++;
    return true;
}

void arm_rto(Tcb *t) {
    if (t->snd_nxt != t->snd_una)
        t->rto_deadline = internal::now_ms() + t->rto_ms;
    else
        t->rto_deadline = 0;
}

// Push unsent bytes while the peer window and flight cap allow.
void pump(Tcb *t) {
    if (t->st != St::EST && t->st != St::CLOSE_WAIT && t->st != St::FIN_W1 &&
        t->st != St::FIN_W2 && t->st != St::LAST_ACK)
        return;
    uint32_t flight = t->snd_nxt - t->snd_una;
    uint32_t wnd = t->snd_wnd;
    if (wnd == 0) wnd = t->peer_mss;  // persist-liveness clamp (documented)
    uint32_t cap = (uint32_t)MAX_FLIGHT_SEGS * t->peer_mss;
    while (flight < wnd && flight < cap) {
        uint32_t unsent_off = flight;  // offset from snd_una in tx[]
        if (unsent_off >= t->tx_len) break;
        uint16_t mss = t->peer_mss;
        uint32_t room_w = wnd - flight;
        uint32_t room_c = cap - flight;
        uint32_t n = t->tx_len - unsent_off;
        if (n > mss) n = mss;
        if (n > room_w) n = room_w;
        if (n > room_c) n = room_c;
        if (n == 0) break;
        bool last = (unsent_off + n >= t->tx_len);
        if (!raw_send(t, F_ACK | (last ? F_PSH : 0), t->snd_nxt, t->tx + unsent_off,
                      (uint16_t)n, false))
            break;
        t->snd_nxt += n;
        flight += n;
    }
    arm_rto(t);
}

void send_ack(Tcb *t) {
    raw_send(t, F_ACK, t->snd_nxt, nullptr, 0, false);
}

void abort_tcb(Tcb *t) {
    raw_send(t, F_RST, t->snd_nxt, nullptr, 0, false);
    t->st = St::CLOSED;
    t->err = -1;
    t->rto_deadline = 0;
}

void graceful_end(Tcb *t) {  // both FINs exchanged (TIME_WAIT skipped, documented)
    t->st = St::CLOSED;
    t->rto_deadline = 0;
}

void on_ack(Tcb *t, uint32_t ack) {
    if (seq_lt(t->snd_nxt, ack)) return;  // ack beyond frontier: drop
    if (seq_lt(ack, t->snd_una)) return;  // duplicate: no progress (data/FIN below still process)
    uint32_t n = ack - t->snd_una;
    if (n > t->tx_len) n = t->tx_len;
    if (n) {
        drop_prefix(t->tx, (uint16_t)n, t->tx_len);
        t->tx_len -= (uint16_t)n;
        t->snd_una = ack;
        t->rto_ms = RTO_INIT;
        t->retries = 0;
    }
    if (t->fin_sent && !t->fin_acked && ack == t->snd_nxt) {
        t->fin_acked = true;
        if (t->st == St::FIN_W1) t->st = St::FIN_W2;
        if (t->st == St::LAST_ACK) graceful_end(t);
    }
    if (t->st == St::FIN_W2 && t->fin_rcvd) graceful_end(t);
    pump(t);
    if (t->snd_nxt == t->snd_una)
        t->rto_deadline = 0;
    else
        arm_rto(t);
}

void on_peer_fin(Tcb *t) {
    if (t->st == St::EST) t->st = St::CLOSE_WAIT;
    if (t->st == St::FIN_W1) t->st = t->fin_acked ? St::CLOSED : St::FIN_W2;
    if (t->st == St::CLOSED) return;
    if (t->st == St::FIN_W2) graceful_end(t);  // peer FIN after ours: done
}

}  // namespace

void poll_timers() {  // called from net::poll (locked) + wait loops
    uint64_t now = internal::now_ms();
    for (int i = 0; i < MAX_CONN; i++) {
        Tcb *t = &s_tcb[i];
        if (!t->used || t->st == St::FREE || t->st == St::CLOSED) continue;
        if (t->rto_deadline == 0 || now < t->rto_deadline) continue;
        if (++t->retries > MAX_RETRIES) {
            abort_tcb(t);
            continue;
        }
        t->rto_ms *= 2;
        if (t->rto_ms > RTO_MAX) t->rto_ms = RTO_MAX;
        s_stats.retrans++;
        if (t->st == St::SYN_SENT) {
            raw_send(t, F_SYN, t->snd_una, nullptr, 0, true);
        } else if (t->st == St::SYN_RCVD) {
            // Passive open: the SYN-ACK itself is outstanding (no data path
            // yet, so pump() would be a no-op). Retransmit it directly.
            raw_send(t, F_SYN | F_ACK, t->snd_una, nullptr, 0, true);
        } else {
            t->snd_nxt = t->snd_una;  // go-back-N rewind
            pump(t);
            if (t->fin_sent && !t->fin_acked && t->snd_nxt == t->snd_una) {
                // FIN itself is outstanding: resend it.
                raw_send(t, F_ACK | F_FIN, t->snd_nxt, nullptr, 0, false);
                t->snd_nxt++;
            }
        }
        arm_rto(t);
    }
}

// RX entry from IPv4 (already under net::poll's lock).
void dispatch(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, uint16_t seg_len) {
    (void)dst_ip;
    if (seg_len < sizeof(Hdr)) {  // (F1)
        s_stats.dropped++;
        return;
    }
    const Hdr *h = (const Hdr *)seg;
    uint8_t hlen = (h->doff >> 4) * 4;
    if (hlen < 20 || hlen > 60 || hlen > seg_len) {  // (F2)
        s_stats.dropped++;
        return;
    }
    // (F3) checksum over pseudo-header + segment.
    {
        static uint8_t tmp[1500];
        if (seg_len > sizeof(tmp)) {
            s_stats.dropped++;
            return;
        }
        util::memcpy(tmp, seg, seg_len);
        tmp[16] = 0;
        tmp[17] = 0;
        if (seg_checksum(src_ip, ip(), tmp, seg_len) != internal::bswap16(h->csum)) {
            s_stats.dropped++;
            return;
        }
    }
    uint16_t sport = internal::bswap16(h->sport);
    uint16_t dport = internal::bswap16(h->dport);
    uint32_t seq = get32be((const uint8_t *)&h->seq);
    uint32_t ack = get32be((const uint8_t *)&h->ack);
    uint8_t flags = h->flags;
    // (F4) options walk (only MSS extracted).
    if (hlen > 20) {
        size_t o = 20;
        bool bad = false;
        while (o < hlen) {
            uint8_t kind = seg[o];
            if (kind == 0) break;  // EOL
            if (kind == 1) {
                o++;
                continue;  // NOP
            }
            if (o + 1 >= hlen) {
                bad = true;
                break;
            }
            uint8_t olen = seg[o + 1];
            if (olen < 2 || o + olen > hlen) {
                bad = true;
                break;
            }
            o += olen;
        }
        if (bad) {
            s_stats.dropped++;
            return;
        }
    }
    Tcb *t = by_quad(src_ip, sport, dport);
    if (!t) {
        // No matching connection: a listener on this port may take the SYN.
        if ((flags & F_SYN) && !(flags & F_ACK)) {
            for (int i = 0; i < MAX_CONN; i++) {
                Tcb *l = &s_tcb[i];
                if (!l->used || l->st != St::LISTEN || l->lport != dport) continue;
                if (l->lip != 0 && l->lip != ip()) continue;
                // Backlog cap: bound half-open + pending-accept children so
                // one scanner cannot exhaust the 8-slot table (SYN drop on
                // overload; the client retransmits).
                int kids = 0;
                for (int k = 0; k < MAX_CONN; k++)
                    if (s_tcb[k].used && s_tcb[k].listener == i) kids++;
                if (kids >= l->backlog) {
                    s_stats.dropped++;
                    return;
                }
                int c = -1;
                for (int k = 0; k < MAX_CONN; k++)
                    if (!s_tcb[k].used) { c = k; break; }
                if (c < 0) break;
                Tcb *ct = &s_tcb[c];
                util::memset(ct, 0, sizeof(Tcb)); // in place: no 32 KiB stack temp
                ct->used = true;
                ct->st = St::SYN_RCVD;
                ct->listener = i;
                ct->lip = ip();
                ct->rip = src_ip;
                ct->lport = dport;
                ct->rport = sport;
                ct->rcv_nxt = seq + 1;
                ct->snd_wnd = OUR_MSS;
                ct->peer_mss = OUR_MSS;
                ct->snd_una = ct->snd_nxt = s_isn;
                s_isn += 0x10001u;
                raw_send(ct, F_SYN | F_ACK, ct->snd_una, nullptr, 0, true);
                ct->snd_nxt++;
                ct->rto_ms = RTO_INIT;
                ct->rto_deadline = internal::now_ms() + ct->rto_ms;
                return;
            }
        }
        s_stats.dropped++;
        return;
    }
    s_stats.rx_segs++;
    if (flags & F_RST) {
        s_stats.rst_rx++;
        t->st = St::CLOSED;
        t->err = -1;
        t->rto_deadline = 0;
        return;
    }
    if (t->st == St::SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == t->snd_una + 1) {
            t->peer_mss = OUR_MSS;
            if (hlen > 20) {  // re-walk for MSS (bounded, same checks)
                size_t o = 20;
                while (o + 1 < hlen) {
                    uint8_t kind = seg[o];
                    if (kind == 0) break;
                    if (kind == 1) {
                        o++;
                        continue;
                    }
                    uint8_t olen = seg[o + 1];
                    if (olen < 2 || o + olen > hlen) break;
                    if (kind == 2 && olen == 4) {
                        uint16_t m = ((uint16_t)seg[o + 2] << 8) | seg[o + 3];
                        if (m >= 536 && m <= 1460) t->peer_mss = m;
                    }
                    o += olen;
                }
            }
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;  // SYN consumed: frontier advances past iss
            t->snd_wnd = internal::bswap16(h->wnd);
            t->st = St::EST;
            t->rto_ms = RTO_INIT;
            t->retries = 0;
            t->rto_deadline = 0;
            send_ack(t);
            pump(t);
        }
        return;
    }
    if (t->st == St::SYN_RCVD) {  // final ACK completing a passive open
        // Duplicate SYN (our SYN-ACK was lost): retransmit SYN-ACK, re-arm.
        if ((flags & F_SYN) && !(flags & F_ACK) && seq == t->rcv_nxt - 1) {
            raw_send(t, F_SYN | F_ACK, t->snd_una, nullptr, 0, true);
            t->rto_deadline = internal::now_ms() + t->rto_ms;
            return;
        }
        if ((flags & F_ACK) && ack == t->snd_una + 1) {
            t->snd_una = ack;
            t->st = St::EST;
            t->rto_deadline = 0;
        }
        return;
    }
    if (t->st == St::CLOSED || t->st == St::FREE) return;
    // Duplicate SYN (our handshake ACK was lost): re-ACK, keep state.
    if ((flags & F_SYN) && seq == t->rcv_nxt - 1 && t->rcv_nxt != 0) {
        send_ack(t);
        return;
    }
    if (flags & F_SYN) return;  // unexpected SYN: drop
    if (flags & F_ACK) on_ack(t, ack);
    uint16_t payload = seg_len - hlen;  // (F5) >= 0 by F2
    const uint8_t *data = seg + hlen;
    if (payload && seq == t->rcv_nxt) {
        uint16_t room = RX_BUF - t->rx_len;
        if (payload > room) {  // (F6) buffer full: drop, keep advertising
            s_stats.dropped++;
        } else {
            util::memcpy(t->rx + t->rx_len, data, payload);
            t->rx_len += payload;
            t->rcv_nxt += payload;
        }
        send_ack(t);
    } else if (payload) {
        send_ack(t);  // out-of-order: dupack (sender fast-retransmits)
    }
    if (flags & F_FIN) {
        if (seq + payload == t->rcv_nxt && !t->fin_rcvd) {
            t->fin_rcvd = true;
            t->rcv_nxt++;
            on_peer_fin(t);
        }
        send_ack(t);
    }
    t->snd_wnd = internal::bswap16(h->wnd);
}

Handle connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    if (!ready() || dst_ip == 0 || dst_port == 0) return -1;
    // ARP prelude (unlocked) so the first SYN goes out immediately.
    {
        uint32_t next = ((dst_ip & mask()) == (ip() & mask())) ? dst_ip : gw();
        internal::arp_resolve(next, 2000);
    }
    int slot = -1;
    {
        internal::Lock guard;
        for (int i = 0; i < MAX_CONN; i++)
            if (!s_tcb[i].used) {
                slot = i;
                break;
            }
        if (slot < 0) return -1;
        uint16_t lport = 0;
        for (int t = 0; t < 64; t++) {
            uint16_t cand = s_next_port++;
            if (s_next_port >= 60000) s_next_port = 40000;
            bool taken = false;
            for (int i = 0; i < MAX_CONN; i++)
                if (s_tcb[i].used && s_tcb[i].lport == cand) {
                    taken = true;
                    break;
                }
            if (!taken) {
                lport = cand;
                break;
            }
        }
        if (!lport) return -1;
        Tcb *t = &s_tcb[slot];
        // Zero in place: a Tcb is ~32 KiB (rx/tx stream buffers), so a
        // `Tcb{}` temporary would overflow the 16 KiB syscall stack
        // (kernel/cpu/linux.asm) and clobber the scheduler globals that sit
        // right below it (s_current, s_count, s_started, s_hook).
        util::memset(t, 0, sizeof(Tcb));
        t->used = true;
        t->st = St::SYN_SENT;
        t->lip = ip();
        t->rip = dst_ip;
        t->lport = lport;
        t->rport = dst_port;
        t->snd_wnd = OUR_MSS;
        t->snd_una = t->snd_nxt = s_isn;
        s_isn += 0x10001u;
        if (!raw_send(t, F_SYN, t->snd_una, nullptr, 0, true)) {
            t->used = false;
            return -1;
        }
        t->snd_nxt++;
        t->rto_deadline = internal::now_ms() + t->rto_ms;
    }
    uint64_t t0 = internal::now_ms();
    while (internal::now_ms() - t0 < timeout_ms) {
        {
            internal::Lock guard;
            Tcb *t = by_handle(slot);
            if (!t) return -1;
            if (t->st == St::EST) return slot;
            if (t->st == St::CLOSED || t->err) {
                t->used = false;
                return -1;
            }
        }
        poll();
        sched::sleep_ms(10);
    }
    internal::Lock guard;
    Tcb *t = by_handle(slot);
    if (t) {
        abort_tcb(t);
        t->used = false;
    }
    return -1;
}

int send(Handle h, const void *data, uint16_t len) {
    if (!data && len) return -1;
    internal::Lock guard;
    Tcb *t = by_handle(h);
    if (!t || t->err) return -1;
    if (t->st != St::EST && t->st != St::CLOSE_WAIT) return -1;
    uint16_t room = TX_BUF - t->tx_len;
    if (room == 0) return 0;
    if (len > room) len = room;  // (F7) partial queue
    util::memcpy(t->tx + t->tx_len, data, len);
    t->tx_len += len;
    pump(t);
    return len;
}

int recv(Handle h, void *buf, uint16_t max, uint32_t timeout_ms) {
    if (!buf || max == 0) return -1;
    uint64_t t0 = internal::now_ms();
    for (;;) {
        {
            internal::Lock guard;
            Tcb *t = by_handle(h);
            if (!t) return -1;
            if (t->rx_len) {
                uint16_t n = t->rx_len < max ? t->rx_len : max;
                util::memcpy(buf, t->rx, n);
                drop_prefix(t->rx, n, t->rx_len);
                t->rx_len -= n;
                if (t->st == St::EST || t->st == St::CLOSE_WAIT) send_ack(t);  // window update
                return n;
            }
            if (t->fin_rcvd) return 0;  // orderly EOF
            if (t->err || t->st == St::CLOSED) return -1;
        }
        if (internal::now_ms() - t0 >= timeout_ms) return -1;
        poll();
        sched::sleep_ms(10);
    }
}

void close(Handle h) {
    uint64_t t0 = internal::now_ms();
    {
        internal::Lock guard;
        Tcb *t = by_handle(h);
        if (!t) return;
        if (t->st == St::LISTEN) {
            // Listener going away: abort + free all its children (half-open
            // and pending-accept) so no orphaned TCB pins a table slot.
            for (int i = 0; i < MAX_CONN; i++) {
                Tcb *c = &s_tcb[i];
                if (!c->used || c->listener != h) continue;
                if (c->st == St::SYN_RCVD) abort_tcb(c);
                c->used = false;
                c->st = St::FREE;
                c->listener = -1;
            }
            t->used = false;  // passive socket: nothing in flight, just free it
            t->st = St::FREE;
            return;
        }
        if (t->st == St::SYN_RCVD) {
            t->used = false;
            t->st = St::FREE;
            return;
        }
        if (t->st == St::SYN_SENT || t->st == St::CLOSED) {
            // Never established: abort immediately, no linger.
            if (t->st == St::SYN_SENT) abort_tcb(t);
            t->used = false;
            t->st = St::FREE;
            return;
        }
        if (t->st == St::EST || t->st == St::CLOSE_WAIT) {
            // Our data is in flight/acked; FIN takes the next sequence number.
            // If unacked data remains it stays queued ahead of the FIN.
            raw_send(t, F_ACK | F_FIN, t->snd_nxt, nullptr, 0, false);
            t->snd_nxt++;
            t->fin_sent = true;
            t->st = (t->st == St::EST) ? St::FIN_W1 : St::LAST_ACK;
            arm_rto(t);
        }
    }
    // Bounded linger for the FIN exchange (F8).
    while (internal::now_ms() - t0 < 5000) {
        {
            internal::Lock guard;
            Tcb *t = by_handle(h);
            if (!t || !t->used || t->st == St::CLOSED || t->st == St::FREE) break;
        }
        poll();
        sched::sleep_ms(10);
    }
    internal::Lock guard;
    Tcb *t = by_handle(h);
    if (t) {
        t->used = false;
        t->st = St::FREE;
    }
}

bool is_open(Handle h) {
    internal::Lock guard;
    Tcb *t = by_handle(h);
    if (!t) return false;
    return t->st == St::EST || t->st == St::FIN_W1 || t->st == St::FIN_W2 ||
           t->st == St::CLOSE_WAIT || t->st == St::LAST_ACK;
}

Handle listen(uint32_t lip, uint16_t port, int backlog) {
    if (!ready() || port == 0) return -1;
    if (backlog < 1) backlog = 1;
    if (backlog > 4) backlog = 4;  // half-open cap; table is only MAX_CONN
    internal::Lock guard;
    for (int i = 0; i < MAX_CONN; i++) {
        if (s_tcb[i].used) continue;
        Tcb *t = &s_tcb[i];
        util::memset(t, 0, sizeof(Tcb));
        t->used = true;
        t->st = St::LISTEN;
        t->lip = lip;
        t->lport = port;
        t->listener = -1;
        t->backlog = backlog;
        return i;
    }
    return -1;
}

Handle accept(Handle lh, uint32_t timeout_ms) {
    uint64_t t0 = internal::now_ms();
    for (;;) {
        {
            internal::Lock guard;
            Tcb *l = by_handle(lh);
            if (!l || l->st != St::LISTEN) return -1;
            // Reap dead children (RST/timed-out SYN_RCVD) so they cannot
            // pin table slots forever; only EST is returned.
            for (int i = 0; i < MAX_CONN; i++) {
                Tcb *c = &s_tcb[i];
                if (!c->used || c->listener != lh) continue;
                if (c->st == St::CLOSED || c->st == St::FREE || c->err) {
                    c->used = false;
                    c->st = St::FREE;
                    c->listener = -1;
                }
            }
            for (int i = 0; i < MAX_CONN; i++) {
                Tcb *c = &s_tcb[i];
                if (c->used && c->listener == lh && c->st == St::EST) {
                    c->listener = -1; // consumed by accept()
                    return i;
                }
            }
        }
        if (internal::now_ms() - t0 >= timeout_ms) return -1;
        poll();
        sched::sleep_ms(10);
    }
}

// poll()/select() readiness: bytes buffered, orderly EOF, or a reset/closed
// peer all make a read succeed/return, so the fd is "readable".
bool readable(Handle h) {
    internal::Lock guard;
    Tcb *t = by_handle(h);
    if (!t) return true;
    return t->rx_len > 0 || t->fin_rcvd || t->err || t->st == St::CLOSED;
}

const char *st_name(St s) {
    switch (s) {
        case St::LISTEN: return "LISTEN";
        case St::SYN_SENT: return "SYN-SENT";
        case St::SYN_RCVD: return "SYN-RCVD";
        case St::EST: return "ESTABLISHED";
        case St::FIN_W1: return "FIN-WAIT-1";
        case St::FIN_W2: return "FIN-WAIT-2";
        case St::CLOSE_WAIT: return "CLOSE-WAIT";
        case St::LAST_ACK: return "LAST-ACK";
        case St::CLOSED: return "CLOSED";
        default: return "-";
    }
}

int conn_count() {
    internal::Lock guard;
    int n = 0;
    for (int i = 0; i < MAX_CONN; i++)
        if (s_tcb[i].used) n++;
    return n;
}

bool conn_at(int i, ConnInfo &out) {
    internal::Lock guard;
    int seen = 0;
    for (int k = 0; k < MAX_CONN; k++) {
        if (!s_tcb[k].used) continue;
        if (seen++ != i) continue;
        const Tcb &t = s_tcb[k];
        out.local_ip = t.lip;
        out.remote_ip = t.rip;
        out.local_port = t.lport;
        out.remote_port = t.rport;
        out.state = st_name(t.st);
        out.rx_pending = t.rx_len;
        out.tx_pending = t.tx_len;
        out.open = t.st == St::EST || t.st == St::FIN_W1 || t.st == St::FIN_W2 ||
                   t.st == St::CLOSE_WAIT || t.st == St::LAST_ACK;
        return true;
    }
    return false;
}

const ConnStats &stats() { return s_stats; }

}  // namespace choco::net::tcp

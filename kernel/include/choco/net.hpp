// ChocoOS network stack (net-1: ARP+IPv4+ICMP, net-2: UDP+DHCP).
// Addresses start at the QEMU user-net defaults and are replaced by the
// DHCP client when it binds (or stay on defaults as fallback).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::net {

// Boot defaults (QEMU user-net) in host byte order. Used until DHCP binds;
// kept as fallback when no server answers.
inline constexpr uint32_t IP_GUEST = (10u | (0u << 8) | (2u << 16) | (15u << 24));
inline constexpr uint32_t IP_GW = (10u | (0u << 8) | (2u << 16) | (2u << 24));
inline constexpr uint32_t IP_MASK = (255u | (255u << 8) | (255u << 16) | (0u << 24));
inline constexpr uint32_t IP_DNS = (10u | (0u << 8) | (2u << 16) | (3u << 24));

enum class IpSource : uint8_t { DEFAULT, DHCP, FALLBACK };
IpSource ip_source();
const char *ip_source_str();  // "default", "dhcp" or "fallback"

void init();  // probe virtio-net, set default config (does NOT block)
void poll();  // drain RX queue, answer ARP/ICMP/UDP. Call often (shell loop).
bool ready();  // true when NIC initialized
bool link_up();

const uint8_t *mac();  // 6 bytes, nullptr when !ready
uint32_t ip();         // current address (default until DHCP binds)
uint32_t gw();
uint32_t mask();
uint32_t dns_server();

// Interface identity for /proc/net/dev. ChocoOS drives one NIC, so the name is
// assigned rather than discovered; "" and 0 mean "no NIC", never a guess.
const char *if_name();
const char *if_driver();  // "virtio-net" when the PCI device was claimed
uint16_t if_mtu();

// Send an IPv4 payload (proto: 1=ICMP, 6=TCP, 17=UDP). Resolves ARP, may
// trigger an ARP request and drop the packet when the MAC is unknown.
// Broadcast destinations skip ARP and go to ff:ff:ff:ff:ff:ff.
bool send_ipv4(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len);
// Same with an explicit source address (DHCP uses 0.0.0.0 pre-bind).
bool send_ipv4_from(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                    const void *payload, uint16_t len);

// ICMP echo request. Blocks up to timeout_ms per packet while polling.
// Returns number of replies received.
int ping(uint32_t dst_ip, int count, uint32_t timeout_ms);

// Parse "a.b.c.d" into host-order u32. Returns false on bad input.
bool parse_ipv4(const char *s, uint32_t &out);
void fmt_ipv4(uint32_t ip, char *out);    // out must hold >= 16 bytes
void fmt_mac(const uint8_t *m, char *out);  // out must hold >= 18 bytes

struct Stats {
    uint64_t rx = 0, tx = 0, dropped = 0;
    uint64_t rx_bytes = 0, tx_bytes = 0;  // whole Ethernet frames, header included
    uint64_t arp_rx = 0, arp_tx = 0;
    uint64_t ip_rx = 0, ip_tx = 0;
    uint64_t icmp_rx = 0, icmp_tx = 0, icmp_reply = 0;
    uint64_t udp_rx = 0, udp_tx = 0, udp_drop = 0;
    uint64_t tcp_rx = 0, tcp_tx = 0, tcp_retrans = 0, tcp_drop = 0;
};
const Stats &stats();

// ARP table inspection for the `arp` shell command.
int arp_count();
bool arp_get(int idx, uint32_t *ip, uint8_t mac_out[6]);
// Same table with the age, for /proc/net/arp. last_ms may be nullptr.
bool arp_at(int idx, uint32_t *ip, uint8_t mac_out[6], uint64_t *last_ms);
void arp_flush();  // drop all entries (forces fresh resolution on next TX)

namespace udp {

// Bounds-audited UDP (RFC 768): every header/option access is length-checked
// before the read; oversize or truncated datagrams are dropped and counted.
// See the audit note at the top of kernel/net/udp.cpp.
inline constexpr uint16_t HDR_SZ = 8;
inline constexpr uint16_t MAX_DGRAM = 1472;  // 1500 - 20 IP - 8 UDP

bool bind(uint16_t port);  // reserve a local port; false when table full/taken
void unbind(uint16_t port);
bool send_to(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
             const void *data, uint16_t len);
// Blocking receive with timeout (polls net::poll internally). Returns payload
// bytes or -1 on timeout/empty. from_ip/from_port optional (may be nullptr).
int recv_from(uint16_t port, void *buf, uint16_t max, uint32_t timeout_ms,
              uint32_t *from_ip, uint16_t *from_port);
// Non-blocking drain used by net::poll dispatch.
bool try_take(uint16_t port, void *buf, uint16_t max, uint16_t &out_len, uint32_t &from_ip,
              uint16_t &from_port);
int socket_count();

struct SockStats {
    uint64_t rx = 0, tx = 0, dropped = 0, csum_err = 0;
};
const SockStats &stats();

// One bound socket, for /proc/net/udp. i counts bound sockets (not slots), so
// it runs 0..socket_count()-1. False past the end.
struct SockInfo {
    uint16_t port = 0;
    bool has_dgram = false;    // a datagram is queued and unread
    uint16_t dgram_len = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
};
bool sock_at(int i, SockInfo &out);

}  // namespace udp

namespace dhcp {

// DHCP client (RFC 2131, strict bounded parsing; see kernel/net/dhcp.cpp).
// Runs in a background thread: bounded retries, never blocks boot.
void start();  // spawn client thread (call once, after sched::init)
bool running();
bool bound();          // true once an ACK was applied
const char *state_str();  // "idle", "discovering", "requesting", "bound", "fallback"
uint32_t server();      // server identifier (0 when unknown)
uint32_t lease_secs();  // granted lease (0 when unknown)
bool renew();           // restart discovery now; false when a run is active

}  // namespace dhcp

namespace dns {

// DNS resolver (UDP/53, A records; see kernel/net/dns.cpp, bounds E1-E8).
// Returns the number of IPv4 addresses stored (0..max_ips), or negative:
// -1 timeout/no route, -2 bad name/args, -3 NXDOMAIN, -4 server failure,
// -5 malformed response. Blocks up to timeout_ms total.
int resolve(const char *name, uint32_t *out_ips, int max_ips, uint32_t timeout_ms);

}  // namespace dns

namespace tcp {

// Minimal TCP client (no listen/accept; see kernel/net/tcp.cpp).
// Memory-bounded: fixed TCB table, 16KB RX/TX stream buffers each.
// Parsing is length-checked (bounds F1-F8 in tcp.cpp). No syscalls here:
// this is the kernel-internal API used by shell test commands (httpget).
// Socket syscalls (socket/connect/send/recv) come in net-5, on top.
using Handle = int;  // 0..MAX_CONN-1, or -1 when invalid
inline constexpr int MAX_CONN = 8;

// Active open. Returns a handle or -1 on timeout/refusal.
Handle connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
// Passive open: bind to `port` on `ip` (0 = any) and listen. Returns a handle.
Handle listen(uint32_t ip, uint16_t port, int backlog);
// Pop a completed inbound connection for listener `lh`. Returns a connected
// handle, or -1 on timeout.
Handle accept(Handle lh, uint32_t timeout_ms);
// Queue bytes for sending (copies into the TCB). Returns bytes queued or -1.
int send(Handle h, const void *data, uint16_t len);
// Receive in-order bytes. Returns bytes, 0 on orderly FIN (EOF), or -1 on
// timeout/error. Never blocks past timeout_ms.
int recv(Handle h, void *buf, uint16_t max, uint32_t timeout_ms);
// Graceful close (FIN exchange, bounded). Always frees the TCB.
void close(Handle h);
// True once ESTABLISHED (or later but not CLOSED/failed).
bool is_open(Handle h);
// Readiness for poll()/select(): true when a recv() would return immediately
// (bytes buffered, EOF, or error).
bool readable(Handle h);

struct ConnStats {
    uint64_t rx_segs = 0, tx_segs = 0, retrans = 0, rst_rx = 0, dropped = 0;
};
const ConnStats &stats();

// One connection, for /proc/net/tcp and the monitor's "connections" view.
// i counts live TCBs, so it runs 0..conn_count()-1.
struct ConnInfo {
    uint32_t local_ip = 0, remote_ip = 0;
    uint16_t local_port = 0, remote_port = 0;
    const char *state = "";   // ESTABLISHED, SYN-SENT, FIN-WAIT-1, ...
    uint16_t rx_pending = 0;  // bytes received and not yet read
    uint16_t tx_pending = 0;  // bytes queued and not yet acknowledged
    bool open = false;
};
int conn_count();
bool conn_at(int i, ConnInfo &out);

}  // namespace tcp

}  // namespace choco::net

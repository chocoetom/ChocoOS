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
uint32_t dns();

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
    uint64_t arp_rx = 0, arp_tx = 0;
    uint64_t ip_rx = 0, ip_tx = 0;
    uint64_t icmp_rx = 0, icmp_tx = 0, icmp_reply = 0;
    uint64_t udp_rx = 0, udp_tx = 0, udp_drop = 0;
};
const Stats &stats();

// ARP table inspection for the `arp` shell command.
int arp_count();
bool arp_get(int idx, uint32_t *ip, uint8_t mac_out[6]);
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

}  // namespace choco::net

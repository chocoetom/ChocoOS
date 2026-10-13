// ChocoOS network stack (Phase net-1: ARP + IPv4 + ICMP).
// Static config for QEMU user networking: guest 10.0.2.15/24,
// gateway 10.0.2.2, DNS 10.0.2.3 (used in later phases).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::net {

// Static addresses in host byte order (a.b.c.d => a | b<<8 | c<<16 | d<<24).
inline constexpr uint32_t IP_GUEST = (10u | (0u << 8) | (2u << 16) | (15u << 24));
inline constexpr uint32_t IP_GW    = (10u | (0u << 8) | (2u << 16) | (2u << 24));
inline constexpr uint32_t IP_MASK  = (255u | (255u << 8) | (255u << 16) | (0u << 24));
inline constexpr uint32_t IP_DNS   = (10u | (0u << 8) | (2u << 16) | (3u << 24));
inline constexpr uint32_t IP_BCAST = (IP_GUEST & IP_MASK) | ~IP_MASK;

void init();   // probe virtio-net, set static IP, post RX buffers
void poll();   // drain RX queue, answer ARP/ICMP. Call often (shell loop).
bool ready();  // true when NIC initialized
bool link_up();

const uint8_t *mac();  // 6 bytes, nullptr when !ready
uint32_t ip();
uint32_t gw();
uint32_t mask();
uint32_t dns();

// Send an IPv4 payload (proto: 1=ICMP, 6=TCP, 17=UDP). Resolves ARP, may
// trigger an ARP request and drop the packet when the MAC is unknown.
bool send_ipv4(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len);

// ICMP echo request. Blocks up to timeout_ms per packet while polling.
// Returns number of replies received.
int ping(uint32_t dst_ip, int count, uint32_t timeout_ms);

// Parse "a.b.c.d" into host-order u32. Returns false on bad input.
bool parse_ipv4(const char *s, uint32_t &out);
void fmt_ipv4(uint32_t ip, char *out);  // out must hold >= 16 bytes
void fmt_mac(const uint8_t *m, char *out);  // out must hold >= 18 bytes

struct Stats {
    uint64_t rx = 0, tx = 0, dropped = 0;
    uint64_t arp_rx = 0, arp_tx = 0;
    uint64_t ip_rx = 0, ip_tx = 0;
    uint64_t icmp_rx = 0, icmp_tx = 0, icmp_reply = 0;
};
const Stats &stats();

// ARP table inspection for the `arp` shell command.
int arp_count();
bool arp_get(int idx, uint32_t *ip, uint8_t mac_out[6]);

}  // namespace choco::net

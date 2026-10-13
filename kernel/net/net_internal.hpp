// Shared internals for kernel/net/*.cpp. NOT public API (see choco/net.hpp).
// Locking: single-core kernel. net::poll() and the public send paths take
// internal::Lock (pushfq/cli, restore IF), so the static TX buffers, the ARP
// table, the UDP socket table and the virtqueue cursors are mutually
// exclusive between the shell thread and the DHCP thread. Blocking waits
// (sleep_ms) always happen OUTSIDE the lock.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <choco/cpu.hpp>

namespace choco::net::internal {

inline uint64_t read_rflags() {
    uint64_t v;
    asm volatile("pushfq; pop %0" : "=r"(v) ::"memory");
    return v;
}

struct Lock {
    uint64_t saved;
    Lock() : saved(read_rflags()) { cpu::cli(); }
    ~Lock() {
        if (saved & 0x200) cpu::sti();
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

inline uint32_t ip_of(const uint8_t b[4]) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

inline void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip & 0xFF);
    out[1] = (uint8_t)((ip >> 8) & 0xFF);
    out[2] = (uint8_t)((ip >> 16) & 0xFF);
    out[3] = (uint8_t)((ip >> 24) & 0xFF);
}

inline uint64_t now_ms() { return pit::ticks() * 10; }  // pit::HZ == 100

// Internet checksum (RFC 1071) over exactly len bytes. Caller bounds len.
inline uint16_t checksum(const void *data, size_t len) {
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

}  // namespace choco::net::internal

// Internal entry: IPv4 layer -> UDP demux. NOT part of the public API;
// implemented in kernel/net/udp.cpp. Caller must have validated that
// dgram points to at least 8 bytes (checked again inside).
namespace choco::net::udp {
void dispatch(uint32_t src_ip, uint32_t dst_ip, const uint8_t *dgram, uint16_t dgram_len);
}  // namespace choco::net::udp

// Internal entries: IPv4 layer -> TCP engine (kernel/net/tcp.cpp).
namespace choco::net::tcp {
void dispatch(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, uint16_t seg_len);
void poll_timers();
}  // namespace choco::net::tcp

// Internal entry: DHCP thread -> dynamic config (kernel/net/net.cpp).
// Single writer (the DHCP thread); plain single-copy stores.
namespace choco::net {
void apply_dhcp_config(uint32_t new_ip, uint32_t new_mask, uint32_t new_gw, uint32_t new_dns);
void apply_fallback_config();
}  // namespace choco::net

namespace choco::net::internal {
// Ensure the ARP entry for next_hop exists (request + bounded wait).
// Returns true when resolved. Never holds the lock across poll().
bool arp_resolve(uint32_t next_hop, uint32_t timeout_ms);
}  // namespace choco::net::internal

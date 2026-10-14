#pragma once
#include <stddef.h>
#include <stdint.h>

// Kernel CSPRNG: a SHA-256/HMAC-DRBG seeded from real boot entropy (RTC, TSC,
// interrupt-timing jitter, and RDRAND/RDSEED when the CPU advertises them).
// This is the source behind getrandom(2) and the AT_RANDOM aux vector, so it
// must be unpredictable, not merely "different every time".
namespace choco::rand {

void init();                                   // seed once, early in boot
void add_entropy(const void *data, size_t n);  // mix caller-supplied entropy
void stir();                                   // sample RDTSC jitter (timer IRQ)
void bytes(void *out, size_t n);               // CSPRNG output bytes
uint64_t u64();                                // one 64-bit word

// True once at least one hardware/timing source was mixed in at boot.
bool seeded();

} // namespace choco::rand

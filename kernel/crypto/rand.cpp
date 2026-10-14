// Kernel CSPRNG. A SHA-256/HMAC SP800-90A-style DRBG fed by real entropy:
// wall clock + TSC + interrupt-timing jitter are mixed at boot and stirred on
// every timer tick, and RDRAND/RDSEED are folded in when CPUID advertises them.
// Output is what getrandom(2) and the ELF loader's AT_RANDOM hand to userland.
//
// Everything here is integer-only (the kernel is built with -mno-sse).
#include <choco/rand.hpp>

#include <choco/cpu.hpp>
#include <choco/rtc.hpp>
#include <choco/util.hpp>

namespace choco::rand {
namespace {

using choco::util::memcpy;
using choco::util::memset;

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct Sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;

    void reset() {
        h[0] = 0x6a09e667; h[1] = 0xbb67ae85; h[2] = 0x3c6ef372; h[3] = 0xa54ff53a;
        h[4] = 0x510e527f; h[5] = 0x9b05688c; h[6] = 0x1f83d9ab; h[7] = 0x5be0cd19;
        len = 0; fill = 0;
    }
    void block(const uint8_t *p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t *p, size_t n) {
        len += n;
        while (n) {
            size_t c = 64 - fill;
            if (c > n) c = n;
            memcpy(buf + fill, p, c);
            fill += c; p += c; n -= c;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    void final(uint8_t out[32]) {
        const uint64_t bits = len * 8;
        uint8_t pad = 0x80, z = 0;
        update(&pad, 1);
        while (fill != 56) update(&z, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(lb, 8);
        for (int i = 0; i < 8; i++) {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
    }
};

struct Hmac {
    Sha256 inner;
    uint8_t opad[64];
    void init(const uint8_t key[32]) {
        uint8_t ipad[64];
        for (int i = 0; i < 32; i++) { ipad[i] = key[i] ^ 0x36; opad[i] = key[i] ^ 0x5c; }
        for (int i = 32; i < 64; i++) { ipad[i] = 0x36; opad[i] = 0x5c; }
        inner.reset();
        inner.update(ipad, 64);
    }
    void update(const uint8_t *p, size_t n) { inner.update(p, n); }
    void final(uint8_t out[32]) {
        uint8_t t[32];
        inner.final(t);
        Sha256 o;
        o.reset();
        o.update(opad, 64);
        o.update(t, 32);
        o.final(out);
    }
};

// --- DRBG state (K/V) + entropy pool ---------------------------------------
uint8_t S_K[32];
uint8_t S_V[32];
uint8_t s_pool[32];
bool s_ready = false;

inline uint64_t irq_save() {
    uint64_t f;
    asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
inline void irq_restore(uint64_t f) {
    asm volatile("pushq %0; popfq" ::"r"(f) : "memory");
}

// SP800-90A HMAC_DRBG update step.
void drbg_update(const uint8_t *seed, size_t seedlen) {
    uint8_t t[32];
    Hmac h;
    h.init(S_K);
    h.update(S_V, 32);
    uint8_t z = 0;
    h.update(&z, 1);
    if (seed) h.update(seed, seedlen);
    h.final(t);
    memcpy(S_K, t, 32);

    Hmac h2;
    h2.init(S_K);
    h2.update(S_V, 32);
    h2.final(S_V);

    if (seed) {
        Hmac h3;
        h3.init(S_K);
        h3.update(S_V, 32);
        uint8_t o = 1;
        h3.update(&o, 1);
        h3.update(seed, seedlen);
        h3.final(t);
        memcpy(S_K, t, 32);

        Hmac h4;
        h4.init(S_K);
        h4.update(S_V, 32);
        h4.final(S_V);
    }
}

void drbg_generate(uint8_t *out, size_t n) {
    while (n) {
        Hmac h;
        h.init(S_K);
        h.update(S_V, 32);
        h.final(S_V);
        size_t c = n < 32 ? n : 32;
        memcpy(out, S_V, c);
        out += c;
        n -= c;
    }
    drbg_update(nullptr, 0);
}

// --- hardware RNG (only when CPUID says the instruction exists) -------------
bool rdrand_ok(uint64_t &v) {
    unsigned char ok;
    asm volatile("rdrand %1; setc %0" : "=r"(ok), "=r"(v) :: "cc");
    return ok;
}
bool rdseed_ok(uint64_t &v) {
    unsigned char ok;
    asm volatile("rdseed %1; setc %0" : "=r"(ok), "=r"(v) :: "cc");
    return ok;
}

uint32_t s_hw = 0; // 1 = RDRAND, 2 = RDSEED
void probe_hw() {
    uint32_t r[4];
    choco::cpu::cpuid(1, 0, r);
    if (r[2] & (1u << 30)) s_hw = 1;
    choco::cpu::cpuid(7, 0, r);
    if (r[1] & (1u << 18)) s_hw = 2;
}

void seed_hardware() {
    for (int i = 0; i < 8; i++) {
        uint64_t v = 0;
        bool got = s_hw == 2 ? rdseed_ok(v) : (s_hw == 1 ? rdrand_ok(v) : false);
        if (got) add_entropy(&v, 8);
    }
}

} // namespace

void add_entropy(const void *data, size_t n) {
    if (!data || !n) return;
    uint64_t f = irq_save();
    // pool = SHA256(pool || data), then fold the pool back into the DRBG.
    Sha256 s;
    s.reset();
    s.update(s_pool, 32);
    s.update((const uint8_t *)data, n);
    s.final(s_pool);
    drbg_update(s_pool, 32);
    irq_restore(f);
}

void init() {
    uint64_t f = irq_save();
    memset(S_K, 0, 32);
    memset(S_V, 1, 32);
    memset(s_pool, 0, 32);

    uint64_t t0 = choco::cpu::rdtsc();
    uint64_t tk = choco::pit::ticks();
    uint64_t ep = choco::rtc::boot_epoch();
    uint64_t addr = (uint64_t)(uintptr_t)&f;
    add_entropy(&t0, 8);
    add_entropy(&tk, 8);
    add_entropy(&ep, 8);
    add_entropy(&addr, 8);

    // Jitter: variable-latency work between TSC reads. On real silicon this is
    // the highest-quality source; on an emulator it is weaker, which is why the
    // hardware RNG and RTC are also folded in.
    uint64_t prev = choco::cpu::rdtsc(), acc = 0;
    for (int i = 0; i < 256; i++) {
        volatile uint64_t x = 0;
        for (int j = 0; j < (i & 31); j++) x += (uint64_t)j;
        uint64_t c = choco::cpu::rdtsc();
        acc ^= (c - prev) * 0x9E3779B97F4A7C15ULL;
        prev = c;
    }
    add_entropy(&acc, 8);
    add_entropy(&prev, 8);

    probe_hw();
    seed_hardware();
    s_ready = true;
    irq_restore(f);
}

void stir() {
    static uint64_t last = 0;
    uint64_t c = choco::cpu::rdtsc();
    uint64_t d = c - last;
    last = c;
    uint64_t s = d ^ (choco::pit::ticks() << 17) ^ c;
    add_entropy(&s, 8);
}

void bytes(void *out, size_t n) {
    if (!out || !n) return;
    uint64_t f = irq_save();
    drbg_generate((uint8_t *)out, n);
    irq_restore(f);
}

uint64_t u64() {
    uint64_t v;
    bytes(&v, 8);
    return v;
}

bool seeded() { return s_ready; }

} // namespace choco::rand

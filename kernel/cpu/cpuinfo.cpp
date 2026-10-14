// CPUID identity + a PIT-measured TSC frequency, for /proc/cpuinfo.
#include <choco/cpuinfo.hpp>

#include <choco/cpu.hpp>

extern "C" bool choco_cpuid_supported(void);

namespace choco::cpuinfo {
namespace {

Info s_info;
bool s_done = false;

void put4(char *dst, uint32_t v) {
    dst[0] = (char)(v & 0xFF);
    dst[1] = (char)((v >> 8) & 0xFF);
    dst[2] = (char)((v >> 16) & 0xFF);
    dst[3] = (char)((v >> 24) & 0xFF);
}

void probe() {
    if (s_done) return;
    s_done = true;
    if (!choco_cpuid_supported()) return;
    uint32_t r[4];

    cpu::cpuid(0, 0, r);
    const uint32_t max_leaf = r[0];
    put4(s_info.vendor + 0, r[1]);
    put4(s_info.vendor + 4, r[3]);
    put4(s_info.vendor + 8, r[2]);
    s_info.vendor[12] = 0;
    s_info.ok = true;

    if (max_leaf >= 1) {
        cpu::cpuid(1, 0, r);
        uint32_t sig = r[0];
        uint32_t fam = (sig >> 8) & 0xF, mod = (sig >> 4) & 0xF;
        s_info.stepping = sig & 0xF;
        if (fam == 0xF) fam += (sig >> 20) & 0xFF;
        if (mod == 0xE || mod == 0xF) mod += ((sig >> 16) & 0xF) << 4;
        s_info.family = fam;
        s_info.model = mod;
        s_info.sse2 = (r[3] & (1u << 26)) != 0;
        s_info.rdtsc = (r[3] & (1u << 4)) != 0;
    }

    cpu::cpuid(0x80000000u, 0, r);
    const uint32_t max_ext = r[0];
    if (max_ext >= 0x80000001u) {
        cpu::cpuid(0x80000001u, 0, r);
        s_info.longmode = (r[3] & (1u << 29)) != 0;
        s_info.nx = (r[3] & (1u << 20)) != 0;
    }
    if (max_ext >= 0x80000004u) {
        for (uint32_t i = 0; i < 3; i++) {
            cpu::cpuid(0x80000002u + i, 0, r);
            for (uint32_t j = 0; j < 4; j++) put4(s_info.brand + i * 16 + j * 4, r[j]);
        }
        s_info.brand[48] = 0;
    }

    // Frequency by timing the counter against the PIT: 5 ticks at 100 Hz is
    // 50 ms, so the alignment error is bounded by 1/5 of the window (~2%).
    //
    // This waits for ticks, so it is only legal with maskable interrupts on.
    // A syscall arrives through an interrupt gate (IF cleared), and halting
    // there never wakes: check first, and bound the wait regardless, so a
    // caller from the wrong context loses the MHz figure instead of the machine.
    if (s_info.rdtsc && cpu::interrupts_on()) {
        const uint64_t t0 = pit::ticks();
        const uint64_t c0 = cpu::rdtsc();
        int spins = 0;
        while (pit::ticks() - t0 < 5 && ++spins < 2000000) cpu::hlt();
        const uint64_t dt = pit::ticks() - t0;
        const uint64_t c1 = cpu::rdtsc();
        if (dt >= 5 && c1 > c0) {
            const uint64_t ms = dt * (1000 / pit::HZ);
            s_info.tsc_khz = (uint32_t)((c1 - c0) / ms);
            s_info.tsc_measured = s_info.tsc_khz != 0;
        }
    }
}

} // namespace

const Info &get() {
    probe();
    return s_info;
}

} // namespace choco::cpuinfo

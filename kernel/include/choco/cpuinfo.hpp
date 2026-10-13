#pragma once
#include <stdint.h>

namespace choco::cpuinfo {

// What CPUID actually reports about this machine, plus a TSC frequency measured
// against the PIT. Everything here is read from hardware; fields that cannot be
// determined stay zero/empty rather than getting a plausible default.
struct Info {
    bool ok = false;         // CPUID is supported and was read
    char vendor[13] = {};    // GenuineIntel / AuthenticAMD / ...
    char brand[49] = {};     // empty when the leaf is absent
    uint32_t family = 0;
    uint32_t model = 0;
    uint32_t stepping = 0;
    bool longmode = false;   // IA-32e (x86_64) support
    bool nx = false;
    bool sse2 = false;
    bool rdtsc = false;
    uint32_t tsc_khz = 0;    // measured, 0 until measured
    bool tsc_measured = false;
};

// Probes on first call. The frequency measurement waits on PIT ticks, so call
// it with interrupts enabled (not during early boot).
const Info &get();

} // namespace choco::cpuinfo

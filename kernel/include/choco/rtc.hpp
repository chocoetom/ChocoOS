#pragma once
#include <stdint.h>

// CMOS real-time clock (mc146818). Read once at boot so the OS has a real
// wall clock instead of only PIT uptime. If the RTC is unreadable everything
// falls back to uptime-based values (see now()).
namespace choco::rtc {

void init();            // read the CMOS clock once; no-op on failure
bool available();       // true when a sane date was read at boot
uint64_t boot_epoch();  // Unix seconds at boot (0 when unavailable)
uint64_t now();         // current Unix time: boot epoch + PIT uptime

} // namespace choco::rtc

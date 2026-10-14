// CMOS real-time clock (mc146818 via ports 0x70/0x71).
//
// Read once at boot into a Unix epoch; afterwards clock_gettime(CLOCK_REALTIME),
// time(2) and gettimeofday advance it with PIT uptime. RTC registers may be BCD
// or binary, 12h or 24h -- the status register B says which. Times are treated
// as UTC (no timezone handling yet).
#include <choco/cpu.hpp>
#include <choco/rtc.hpp>

namespace choco::rtc {
namespace {

uint64_t s_boot = 0;
bool s_ok = false;

uint8_t cmos_read(uint8_t reg) {
    cpu::outb(0x70, reg);
    return cpu::inb(0x71);
}
// Status register A, bit 7: an update is in progress; don't read the clock now.
bool update_in_progress() { return (cmos_read(0x0A) & 0x80) != 0; }
int from_bcd(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Integer only -- the kernel has no FPU.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

} // namespace

void init() {
    for (int i = 0; i < 1000000 && update_in_progress(); i++) {}
    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t yr = cmos_read(0x09);
    const uint8_t regb = cmos_read(0x0B);
    if (update_in_progress()) return; // clock changed under us: keep uptime fallback

    if (!(regb & 0x04)) { // BCD encoded
        sec = (uint8_t)from_bcd(sec);
        min = (uint8_t)from_bcd(min);
        hour = (uint8_t)(from_bcd((uint8_t)(hour & 0x7F)) | (hour & 0x80));
        day = (uint8_t)from_bcd(day);
        mon = (uint8_t)from_bcd(mon);
        yr = (uint8_t)from_bcd(yr);
    }
    if (!(regb & 0x02)) { // 12-hour mode -> 24-hour
        const bool pm = (hour & 0x80) != 0;
        hour = (uint8_t)(hour & 0x7F);
        if (pm && hour != 12) hour = (uint8_t)(hour + 12);
        if (!pm && hour == 12) hour = 0;
    }
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return;

    const int year = 2000 + yr; // sane for 2000-2099 (enough for this OS)
    const int64_t days = days_from_civil(year, mon, day);
    s_boot = (uint64_t)(days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + sec);
    s_ok = true;
}

bool available() { return s_ok; }
uint64_t boot_epoch() { return s_boot; }
uint64_t now() { return s_ok ? s_boot + pit::seconds() : pit::seconds(); }

} // namespace choco::rtc

// PIT 8253/8254 channel 0 as the system clock (100 Hz).
#include <choco/cpu.hpp>

namespace choco::pit {
namespace {
volatile uint64_t s_ticks = 0;
} // namespace

void init() {
    constexpr uint32_t BASE = 1193182;
    constexpr uint16_t div = (uint16_t)(BASE / HZ);
    cpu::outb(0x43, 0x36); // channel 0, lo/hi, mode 3 (square wave)
    cpu::outb(0x40, (uint8_t)(div & 0xFF));
    cpu::outb(0x40, (uint8_t)(div >> 8));
}

void on_tick() { s_ticks = s_ticks + 1; }
uint64_t ticks() { return s_ticks; }
uint64_t seconds() { return s_ticks / HZ; }

} // namespace choco::pit

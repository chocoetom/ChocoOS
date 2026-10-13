// PCI implementation
#include <choco/pci.hpp>
#include <choco/cpu.hpp>
#include <choco/console.hpp>

using choco::cpu::inl;
using choco::cpu::outl;
using choco::console::FG;
using choco::console::BG;

namespace choco::pci {

uint64_t bar_base(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_idx) {
    if (bar_idx > 5) return 0;
    uint32_t bar = read_config32(bus, slot, func, 0x10 + bar_idx * 4);
    if (!(bar & 1)) {
        // Memory BAR
        return bar & 0xFFFFFFF0;
    } else {
        // IO BAR
        return bar & 0xFFFFFFFC;
    }
}

void enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t cmd = read_config16(bus, slot, func, 0x04);
    cmd |= (1 << 2) | (1 << 1) | (1 << 0); // Bus master, memory, IO
    write_config16(bus, slot, func, 0x04, cmd);
    choco::console::kprintf(FG, BG, "[pci] Bus master enabled for %x:%x.%d\n", bus, slot, func);
}

} // namespace choco::pci
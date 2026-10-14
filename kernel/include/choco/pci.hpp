// PCI enumeration and configuration space access
#pragma once
#include <stdint.h>
#include <choco/cpu.hpp>

namespace choco::pci {

// Read/write PCI config space via CF8/CFC ports
static inline uint32_t read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
    choco::cpu::outl(0xCF8, address);
    return choco::cpu::inl(0xCFC);
}

static inline uint16_t read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = read_config32(bus, slot, func, offset & 0xFC);
    return (uint16_t)(v >> ((offset & 2) * 8));
}

static inline uint8_t read_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = read_config32(bus, slot, func, offset & 0xFC);
    return (uint8_t)(v >> ((offset & 3) * 8));
}

static inline void write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
    choco::cpu::outl(0xCF8, address);
    choco::cpu::outl(0xCFC, value);
}

static inline void write_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t v = read_config32(bus, slot, func, offset & 0xFC);
    v &= ~(0xFFFFu << ((offset & 2) * 8));
    v |= (uint32_t)value << ((offset & 2) * 8);
    write_config32(bus, slot, func, offset & 0xFC, v);
}

static inline void write_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    uint32_t v = read_config32(bus, slot, func, offset & 0xFC);
    v &= ~(0xFFu << ((offset & 3) * 8));
    v |= (uint32_t)value << ((offset & 3) * 8);
    write_config32(bus, slot, func, offset & 0xFC, v);
}

// BAR handling
uint64_t bar_base(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_idx);

// Enable bus mastering and memory/IO space
void enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func);

// Common PCI class codes
constexpr uint8_t CLASS_NETWORK = 0x02;
constexpr uint8_t CLASS_BLOCK = 0x01;

} // namespace choco::pci
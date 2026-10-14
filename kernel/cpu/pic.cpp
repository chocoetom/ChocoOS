// PIC 8259: remap to vectors 32..47, EOI and masks.
#include <choco/cpu.hpp>

namespace choco::pic {
namespace {
constexpr uint16_t M_CMD = 0x20, M_DAT = 0x21;
constexpr uint16_t S_CMD = 0xA0, S_DAT = 0xA1;
uint8_t s_mmask = 0xFF, s_smask = 0xFF;

void write_mask() {
    cpu::outb(M_DAT, s_mmask);
    cpu::outb(S_DAT, s_smask);
}
} // namespace

void remap() {
    // Save masks, ICW1..ICW4, restore with timer+keyboard unmasked.
    s_mmask = cpu::inb(M_DAT);
    s_smask = cpu::inb(S_DAT);
    (void)s_mmask; (void)s_smask;

    cpu::outb(M_CMD, 0x11); cpu::outb(S_CMD, 0x11);
    cpu::outb(M_DAT, 32);   cpu::outb(S_DAT, 40);
    cpu::outb(M_DAT, 0x04); cpu::outb(S_DAT, 0x02);
    cpu::outb(M_DAT, 0x01); cpu::outb(S_DAT, 0x01);

    s_mmask = 0xFC; // bit0 (timer) and bit1 (keyboard) free, rest masked
    s_smask = 0xFF; // slave fully masked here; mouse::init unmasks IRQ12
    write_mask();
}

void eoi(uint8_t irq) {
    if (irq >= 8) cpu::outb(S_CMD, 0x20);
    cpu::outb(M_CMD, 0x20);
}

void mask(uint8_t irq) {
    if (irq < 8) s_mmask |= (uint8_t)(1 << irq);
    else s_smask |= (uint8_t)(1 << (irq - 8));
    write_mask();
}

void unmask(uint8_t irq) {
    if (irq < 8) s_mmask &= (uint8_t)~(1 << irq);
    else s_smask &= (uint8_t)~(1 << (irq - 8));
    write_mask();
}

} // namespace choco::pic

// Serial COM1 (0x3F8) for logs under QEMU -serial stdio
#include <choco/serial.hpp>
#include <stdint.h>

namespace {
inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
inline uint8_t inb(uint16_t p) { uint8_t r; asm volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
constexpr uint16_t COM1 = 0x3F8;
} // namespace

namespace choco::serial {

void init() {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void write_char(char c) {
    if (c == '\n') write_char('\r');
    while ((inb(COM1 + 5) & 0x20) == 0) {}
    outb(COM1, (uint8_t)c);
}

void write(const char *s) {
    for (; *s; s++) write_char(*s);
}

} // namespace choco::serial

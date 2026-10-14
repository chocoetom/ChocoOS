#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::cpu {

// --- ports / flags ---
inline uint8_t inb(uint16_t p) { uint8_t r; asm volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
inline uint16_t inw(uint16_t p) { uint16_t r; asm volatile("inw %1,%0" : "=a"(r) : "Nd"(p)); return r; }
inline void outw(uint16_t p, uint16_t v) { asm volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
inline uint32_t inl(uint16_t p) { uint32_t r; asm volatile("inl %1,%0" : "=a"(r) : "Nd"(p)); return r; }
inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
inline void cli() { asm volatile("cli" ::: "memory"); }
inline void sti() { asm volatile("sti" ::: "memory"); }
inline void hlt() { asm volatile("hlt"); }
// True when maskable interrupts are enabled. A syscall enters through an
// interrupt gate, so inside one this is false and hlt() never wakes.
inline bool interrupts_on() {
    uint64_t f;
    asm volatile("pushfq; pop %0" : "=r"(f));
    return (f & 0x200) != 0;
}
inline uint16_t read_cs() { uint16_t v; asm volatile("mov %%cs,%0" : "=r"(v)); return v; }

// Raw instructions that clobber rbx, so they live in cpu/cpuid.asm (C linkage).
extern "C" void choco_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]);
extern "C" uint64_t choco_rdtsc(void);
inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    choco_cpuid(leaf, sub, out);
}
inline uint64_t rdtsc() { return choco_rdtsc(); }

// Frame delivered by the ASM stub (see cpu/isr.asm for the push order).
struct Frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err;
    uint64_t rip, cs, rflags, rsp, ss;
};

} // namespace choco::cpu

namespace choco::idt {
void init(); // builds IDT 0..47 and lidt (no sti)
uint64_t irq_count(uint64_t vec); // counters 0..47
void dump_stats();
} // namespace choco::idt

namespace choco::pic {
inline constexpr uint8_t IRQ_TIMER = 0, IRQ_KBD = 1;
void remap();              // offsets 32/40, mask everything but timer+keyboard
void eoi(uint8_t irq);     // 0..15
void mask(uint8_t irq);
void unmask(uint8_t irq);
} // namespace choco::pic

namespace choco::pit {
inline constexpr uint32_t HZ = 100;
void init();               // channel 0, HZ ticks/s
void on_tick();            // called by IRQ0
uint64_t ticks();
uint64_t seconds();
} // namespace choco::pit

namespace choco::kbd {
// Ring buffer fed by IRQ1, consumed by the main loop.
void init();               // clears the buffer (and drops any pending byte)
void on_irq();             // called by IRQ1: reads 0x60, translates, enqueues
bool pop(char &out);       // false when empty (thread context, IRQ-safe)
bool try_pop(char &out);   // lock-free pop for IRQ-off contexts (tick hook, syscalls)
void unget(char c);        // push one byte back (only right after a pop)
uint64_t total_keys();

// Special keys (0x80+, never collide with translated ASCII).
inline constexpr char KEY_UP = (char)0x80;
inline constexpr char KEY_DOWN = (char)0x81;
} // namespace choco::kbd

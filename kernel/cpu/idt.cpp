// IDT with 48 gates + exception/IRQ dispatch (all output in English).
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/mem.hpp>
#include <choco/mouse.hpp>
#include <choco/sched.hpp>
#include <choco/serial.hpp>
#include <choco/user.hpp>

using choco::console::ACC;
using choco::console::BG;
using choco::console::ERR;
using choco::console::FG;
using choco::console::OK;

// ASM stubs (cpu/isr.asm)
extern "C" {
void isr0(); void isr1(); void isr2(); void isr3(); void isr4(); void isr5();
void isr6(); void isr7(); void isr8(); void isr9(); void isr10(); void isr11();
void isr12(); void isr13(); void isr14(); void isr15(); void isr16(); void isr17();
void isr18(); void isr19(); void isr20(); void isr21(); void isr22(); void isr23();
void isr24(); void isr25(); void isr26(); void isr27(); void isr28(); void isr29();
void isr30(); void isr31(); void isr32(); void isr33(); void isr34(); void isr35();
void isr36(); void isr37(); void isr38(); void isr39(); void isr40(); void isr41();
void isr42(); void isr43(); void isr44(); void isr45(); void isr46(); void isr47();
void isr128(); // ring-3 syscall gate (int $0x80)
}

namespace choco::idt {
namespace {

struct Entry {
    uint16_t off_lo; uint16_t sel; uint8_t ist; uint8_t attr;
    uint16_t off_mid; uint32_t off_hi; uint32_t zero;
};
struct Ptr { uint16_t limit; uint64_t base; } __attribute__((packed));

Entry s_idt[256] = {};
uint64_t s_count[256] = {};
Ptr s_ptr{};

void set_gate(uint8_t n, void (*fn)(), uint16_t cs, uint8_t ist = 0, uint8_t dpl = 0) {
    uint64_t a = (uint64_t)fn;
    s_idt[n].off_lo = (uint16_t)a;
    s_idt[n].sel = cs;
    s_idt[n].ist = ist;
    s_idt[n].attr = (uint8_t)(0x8E | ((dpl & 3) << 5)); // present, 64-bit int gate, DPL
    s_idt[n].off_mid = (uint16_t)(a >> 16);
    s_idt[n].off_hi = (uint32_t)(a >> 32);
    s_idt[n].zero = 0;
}

const char *exc_name(uint64_t v) {
    static const char *k[] = {
        "#DE divide by zero", "#DB debug", "NMI", "#BP breakpoint",
        "#OF overflow", "#BR bound range", "#UD invalid opcode", "#NM no coprocessor",
        "#DF double fault", "reserved(9)", "#TS invalid TSS", "#NP segment not present",
        "#SS stack fault", "#GP general protection", "#PF page fault", "reserved(15)",
        "#MF x87 fault", "#AC alignment check", "#MC machine check", "#XM SIMD fault",
        "#VE virtualization", "#CP control protection", "reserved(22)", "reserved(23)",
        "reserved(24)", "reserved(25)", "reserved(26)", "reserved(27)",
        "#HV hypervisor", "#VC VMM communication", "#SX security", "reserved(31)",
    };
    return v < 32 ? k[v] : "?";
}

void dump_frame(const cpu::Frame *f) {
    console::kprintf(FG, BG, " vec=%u err=0x%lx rip=0x%lx cs=0x%lx rflags=0x%lx\n",
                     (unsigned)f->vec, f->err, f->rip, f->cs, f->rflags);
    console::kprintf(FG, BG, " rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    console::kprintf(FG, BG, " rsi=0x%lx rdi=0x%lx rbp=0x%lx rsp=0x%lx\n", f->rsi, f->rdi, f->rbp, f->rsp);
    if (f->vec == 14) {
        uint64_t cr2; asm volatile("mov %%cr2,%0" : "=r"(cr2));
        console::kprintf(FG, BG, " cr2 (fault address)=0x%lx\n", cr2);
    }
}

} // namespace

void init() {
    uint16_t cs = cpu::read_cs(); // our own GDT must be live before this call
    void (*stubs[48])() = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,
        isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,
        isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31,
        isr32,isr33,isr34,isr35,isr36,isr37,isr38,isr39,isr40,isr41,
        isr42,isr43,isr44,isr45,isr46,isr47,
    };
    for (uint8_t i = 0; i < 48; i++)
        set_gate(i, stubs[i], cs, i == 8 ? 1 : 0); // #DF runs on IST1
    set_gate(128, isr128, cs, 0, 3); // syscall gate callable from ring 3
    s_ptr.limit = sizeof(s_idt) - 1;
    s_ptr.base = (uint64_t)&s_idt;
    asm volatile("lidt %0" ::"m"(s_ptr) : "memory");
}

uint64_t irq_count(uint64_t v) { return v < 256 ? s_count[v] : 0; }

void dump_stats() {
    console::println("-- IRQs --", ACC);
    console::kprintf(FG, BG, " timer(32)=%llu kbd(33)=%llu\n", s_count[32], s_count[33]);
}

} // namespace choco::idt

// Dispatch called by the ASM stub with interrupts disabled by the CPU.
extern "C" void isr_dispatch(choco::cpu::Frame *f) {
    using namespace choco;
    if (f->vec < 256) idt::s_count[f->vec]++;

    if (f->vec == 128) {
        syscall::handle(f->vec, f); // no EOI: software trap, return value in rax
        return;
    }
    if (f->vec == 32) {
        pit::on_tick();
        pic::eoi(0);
        sched::tick(); // preemptive round-robin (transparent to the interrupted thread)
        return;
    }
    if (f->vec == 33) {
        kbd::on_irq();
        pic::eoi(1);
        return;
    }
    if (f->vec == 44) {
        mouse::on_irq();
        pic::eoi(12);
        return;
    }
    if (f->vec >= 32 && f->vec < 48) {
        pic::eoi((uint8_t)(f->vec - 32)); // unused / spurious IRQ
        return;
    }
    // Exception. A ring-3 fault (bad pointer, #UD, #GP...) must NOT take the
    // kernel down: terminate the offending process (SIGSEGV-style) and let the
    // scheduler keep running. Only a kernel-mode fault is fatal.
    if ((f->cs & 3) == 3) {
        serial::write("\n[EXC] ring-3 fault: killing task\n");
        console::kprintf(ERR, console::BG, "\n[EXC] %s in ring 3 -> killing task\n",
                         idt::exc_name(f->vec));
        idt::dump_frame(f); // diagnostics: rip/cr2/rsp of the faulting user task
        proc::exit_current(128 + 11); // 128 + SIGSEGV; never returns
    }
    cpu::cli();
    serial::write("\n[EXC] ");
    serial::write(idt::exc_name(f->vec));
    serial::write("\n");
    console::kprintf(ERR, console::BG, "\n[EXC] %s\n", idt::exc_name(f->vec));
    idt::dump_frame(f);
    console::println("Choco is lying belly-up. Reboot (QEMU: Ctrl+Alt+G / reset).", ERR);
    for (;;) { asm volatile("hlt"); }
}

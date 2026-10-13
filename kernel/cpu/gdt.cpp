// Long-mode GDT + TSS (all messages/comments in English by convention).
#include <choco/console.hpp>
#include <choco/sched.hpp> // only for header dir consistency
#include <choco/util.hpp>

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;

// Implemented in gdt.asm
extern "C" void gdt_load(const void *gdtr, uint16_t code_sel, uint16_t data_sel, uint16_t tss_sel);

namespace choco::gdt {
namespace {

struct Tss {
    uint32_t reserved0 = 0;
    uint64_t rsp0 = 0, rsp1 = 0, rsp2 = 0, reserved1 = 0;
    uint64_t ist1 = 0, ist2 = 0, ist3 = 0, ist4 = 0, ist5 = 0, ist6 = 0, ist7 = 0;
    uint64_t reserved2 = 0;
    uint16_t reserved3 = 0, iomap = 0;
} __attribute__((packed));

struct Gdtr { uint16_t limit; uint64_t base; } __attribute__((packed));

__attribute__((aligned(16))) uint64_t s_gdt[7] = {};
__attribute__((aligned(16))) Tss s_tss{};
__attribute__((aligned(16))) uint8_t s_rsp0_stack[16384] = {};
__attribute__((aligned(16))) uint8_t s_ist1_stack[16384] = {};
Gdtr s_gdtr{};

constexpr uint16_t SEL_KCODE = 0x08, SEL_KDATA = 0x10;
constexpr uint16_t SEL_UCODE = 0x18, SEL_UDATA = 0x20;
constexpr uint16_t SEL_TSS = 0x28;
// usermode.asm hardcodes UCODE|3 and UDATA|3: keep them in sync.
static_assert(SEL_UCODE + 3 == 0x1B && SEL_UDATA + 3 == 0x23, "usermode.asm selectors");

uint64_t seg(uint32_t access, uint32_t flags) {
    // base=0, limit=0xFFFFF (ignored in long mode, G=1 for compatibility)
    return 0xFFFFULL | ((uint64_t)access << 40) | ((uint64_t)(flags & 0xF) << 52) | ((uint64_t)0xF << 48);
}

} // namespace

uint16_t code_selector() { return SEL_KCODE; }
uint16_t tss_selector() { return SEL_TSS; }

// Point the interrupt stack (rsp0) at the running thread's kernel stack. The
// scheduler calls this on every switch: a single shared rsp0 made two threads'
// timer frames land on the same address, so a resumed thread read back the
// other's frame. 0 restores the boot default.
void set_rsp0(uint64_t top) {
    s_tss.rsp0 = top ? top : (uint64_t)&s_rsp0_stack[sizeof(s_rsp0_stack)];
}

void init() {
    util::memset(s_gdt, 0, sizeof(s_gdt));
    s_gdt[0] = 0;
    s_gdt[1] = seg(0x9A, 0xA); // kernel code: P,DPL0,S,exec/read,L=1
    s_gdt[2] = seg(0x92, 0xC); // kernel data: P,DPL0,S,read/write,D=1
    s_gdt[3] = seg(0xFA, 0xA); // user code (phase 5)
    s_gdt[4] = seg(0xF2, 0xC); // user data (phase 5)

    util::memset(&s_tss, 0, sizeof(s_tss));
    s_tss.rsp0 = (uint64_t)&s_rsp0_stack[sizeof(s_rsp0_stack)];
    s_tss.ist1 = (uint64_t)&s_ist1_stack[sizeof(s_ist1_stack)];
    s_tss.iomap = sizeof(s_tss);

    uint64_t base = (uint64_t)&s_tss;
    uint32_t lim = sizeof(s_tss) - 1;
    s_gdt[5] = (lim & 0xFFFFULL) | ((base & 0xFFFFULL) << 16) | (((base >> 16) & 0xFFULL) << 32) |
               ((uint64_t)0x89 << 40) | (((uint64_t)(((uint32_t)lim >> 16) & 0xF)) << 48) | (((base >> 24) & 0xFFULL) << 56);
    s_gdt[6] = base >> 32;

    s_gdtr.limit = sizeof(s_gdt) - 1;
    s_gdtr.base = (uint64_t)&s_gdt;

    gdt_load(&s_gdtr, SEL_KCODE, SEL_KDATA, SEL_TSS);
    console::kprintf(FG, BG, "[gdt] own table live, cs=0x%x tss=0x%x rsp0=%p ist1=%p\n",
                     SEL_KCODE, SEL_TSS, (void *)s_tss.rsp0, (void *)s_tss.ist1);
}

} // namespace choco::gdt

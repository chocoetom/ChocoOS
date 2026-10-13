// ChocoOS v0.6 - Phase 5: user mode + syscalls + initrd (all output in English)
#include <choco/boot.hpp>
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/gui.hpp>
#include <choco/mem.hpp>
#include <choco/mouse.hpp>
#include <choco/sched.hpp>
#include <choco/shell.hpp>
#include <choco/user.hpp>
#include <choco/serial.hpp>
#include <choco/net.hpp>
#include <choco/vfs.hpp>

namespace vfs = choco::vfs;

namespace con = choco::console;
using con::BG; using con::FG; using con::OK; using con::ERR;

static void hcf() { choco::cpu::cli(); for (;;) { asm volatile("hlt"); } }

extern "C" void kmain() {
    if (!choco::boot::collect()) hcf();
    const auto &bi = choco::boot::info();

    choco::cpu::cli();
    choco::serial::init();
    choco::serial::write("ChocoOS 0.6 boot\n");

    con::early_init(bi.fb_addr, bi.fb_w, bi.fb_h, bi.fb_pitch_px);
    con::banner("ChocoOS 0.6 - powered by Choco the black cat");

    con::println("  /\\_/\\", FG);
    con::kprintf(FG, BG, " ( o.o )  ChocoOS 0.6: phase 5 - user mode\n");
    con::println("  > ^ <", FG);

    // --- Phase 1: memory ---
    choco::pmm::init();
    con::kprintf(OK, BG, "[pmm] total=%u MiB free=%u MiB\n",
                 (unsigned)(choco::pmm::total_bytes() / 1048576),
                 (unsigned)(choco::pmm::free_bytes() / 1048576));
    choco::pmm::dump_map();
    choco::paging::init();
    con::println("[post] paging::init returned", OK);
    con::kprintf(OK, BG, "[paging] PML4=0x%lx\n", choco::paging::current_pml4_phys());
    choco::heap::init();
    con::println("[post] heap::init returned", OK);
    if (!choco::heap::self_test()) { con::println("[heap] FAILED", ERR); hcf(); }
    con::println("[heap] self-test OK", OK);

    // --- Phase 2: interrupts (GDT first so the IDT uses our code selector) ---
    choco::gdt::init();
    choco::idt::init();
    choco::pic::remap();
    choco::pit::init();
    choco::kbd::init();
    choco::mouse::init();
    choco::linux::init(); // EFER.SCE + SYSCALL entry (Linux translator)
    con::kprintf(OK, BG, "[idt] 49 vectors (0x80 = ring-3 syscalls, cs=0x%x) | [pic] 32/40 | [pit] %u Hz | [kbd] ps/2\n",
                 choco::cpu::read_cs(), choco::pit::HZ);

    // --- Phase 5: VFS + initrd, then scheduler + shell (never returns) ---
    choco::fs::init();

    // Mount the initrd as root filesystem
    if (vfs::vfs_mount("/", "initrd", nullptr)) {
        con::println("[vfs] initrd mounted at /");
    } else {
        con::println("[vfs] initrd mount failed", con::ERR);
    }

    // Mount procfs for system info
    if (vfs::vfs_mount("/proc", "procfs", nullptr)) {
        con::println("[vfs] proc mounted at /proc");
    } else {
        con::println("[vfs] proc mount failed", con::ERR);
    }

    con::println("[vfs] VFS layer active");

    // Network (net-1: ARP/IPv4/ICMP). Owns the virtio-net device.
    choco::net::init();

    choco::sched::init();
    choco::shell::start_workers();
    choco::gui::init(); // desktop takes over the console surface
    choco::sched::set_started(true);
    choco::cpu::sti();
    choco::shell::run();
}

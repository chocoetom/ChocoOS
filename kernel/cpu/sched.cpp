// Round-robin kernel scheduler (single core, preemptive via PIT).
// Switching inside the timer IRQ is safe: only callee-saved registers move,
// each thread's interrupt Frame stays on its own stack until iret.
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/mem.hpp>
#include <choco/paging.hpp>
#include <choco/sched.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

using choco::console::ACC;
using choco::console::BG;
using choco::console::FG;
using choco::console::OK;

extern "C" {
void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
void thread_trampoline();
void choco_thread_exit();
// Per-thread syscall entry stack (see kernel/cpu/linux.asm): switched here so
// two threads' syscalls never share one frame.
extern uint64_t linux_syscall_rsp;
}

namespace choco::sched {
namespace {

constexpr size_t MAX_THREADS = 16;
constexpr uint64_t QUANTUM_TICKS = 5; // 50 ms at 100 Hz

enum : uint8_t { ST_FREE = 0, ST_RUN = 1, ST_DEAD = 2 };

struct Thread {
    uint64_t rsp = 0;
    uint32_t pid = 0;
    uint32_t pgid = 0;
    uint32_t parent_pid = 0;
    long exit_status = 0;
    const char *name = "";
    uint8_t state = ST_FREE;
    uint64_t runs = 0;
    uint64_t cpu_ticks = 0;  // timer ticks spent running: 1 tick = 10 ms at HZ=100
    uint64_t stack_phys = 0;
    size_t stack_pages = 0;
    uint64_t syscall_stack = 0; // per-thread syscall entry stack top (user threads)
    uint64_t kstack_top = 0;    // per-thread kernel/interrupt stack top (rsp0)
    alignas(16) uint8_t fpu[512] = {}; // FXSAVE area: per-thread x87 + SSE state
    uint64_t cr3 = 0; // address space; kernel threads share the boot PML4
    bool owns_cr3 = false;
    bool idle_task = false;  // the boot thread: its time is idle, not busy
};

Thread s_threads[MAX_THREADS]{};
Thread *s_current = nullptr;
size_t s_count = 0;
uint64_t s_switches = 0;
uint64_t s_last_tick = 0;
uint32_t s_next_pid = 1;
bool s_started = false;
TickHook s_hook = nullptr;
uint64_t s_kernel_cr3 = 0;

Thread *pick_next() {
    if (s_count == 0) return s_current;
    size_t idx = 0;
    for (size_t i = 0; i < MAX_THREADS; i++)
        if (&s_threads[i] == s_current) { idx = i; break; }
    for (size_t n = 1; n <= MAX_THREADS; n++) {
        Thread *t = &s_threads[(idx + n) % MAX_THREADS];
        if (t->state == ST_RUN) return t;
    }
    return s_current;
}

void do_switch() {
    Thread *cur = s_current;
    Thread *nxt = pick_next();
    if (nxt == cur) return;
    // Save the outgoing thread's FPU/SSE state; after switch_context resumes on
    // the incoming thread's stack, s_current names that thread, so restore its
    // area. Without this, two threads using float share one FPU and corrupt each
    // other's registers across a preemption.
    asm volatile("fxsave (%0)" ::"r"(cur->fpu) : "memory");
    s_current = nxt;
    nxt->runs++;
    s_switches++;
    if (nxt->syscall_stack) linux_syscall_rsp = nxt->syscall_stack;
    gdt::set_rsp0(nxt->kstack_top);
    linux::switch_fs(nxt->pid); // another process may have moved the FS base
    if (nxt->cr3 && nxt->cr3 != paging::read_cr3()) paging::write_cr3(nxt->cr3);
    switch_context(&cur->rsp, nxt->rsp);
    // Resumed on the incoming thread's stack: s_current now names THAT thread
    // (set before the switch), which is what must be restored here.
    asm volatile("fxrstor (%0)" ::"r"(s_current->fpu) : "memory");
}

// A private 16 KiB syscall stack per thread, so one thread's syscall frame is
// never overwritten by another's (the old single global stack broke as soon as
// a blocking syscall let a second thread run).
uint64_t alloc_syscall_stack() {
    static uint64_t s_sstack_virt = 0xFFFFFFFFD0000000ULL;
    constexpr size_t kSyscallStackPages = 4;
    uint64_t vbase = s_sstack_virt;
    s_sstack_virt += kSyscallStackPages * 4096 + 4096; // + guard gap
    for (size_t p = 0; p < kSyscallStackPages; p++) {
        uint64_t ph = pmm::alloc_zero();
        if (!ph) return 0;
        if (!paging::map(vbase + p * 4096, ph, paging::F_PRESENT | paging::F_RW)) {
            pmm::free(ph);
            return 0;
        }
    }
    return vbase + kSyscallStackPages * 4096;
}

} // namespace

void init() {
    util::memset(s_threads, 0, sizeof(s_threads));
    s_threads[0].name = "idle";
    s_threads[0].pid = 1;
    s_threads[0].state = ST_RUN;
    s_threads[0].runs = 1;
    s_threads[0].idle_task = true;
    s_current = &s_threads[0];
    s_count = 1;
    s_switches = 0;
    s_last_tick = pit::ticks();
    s_kernel_cr3 = paging::current_pml4_phys();
    s_threads[0].cr3 = s_kernel_cr3;
    console::println("[sched] idle thread registered", OK);
}

void set_started(bool on) { s_started = on; }

bool spawn(const char *name, Entry entry, void *arg, size_t stack_pages) {
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (s_threads[i].state != ST_FREE) continue;
        // Fresh virtual run above the heap; back it with PMM pages on demand.
        static uint64_t s_virt = 0xFFFFFFFFB0000000ULL;
        uint64_t vbase = s_virt;
        s_virt += stack_pages * 4096 + 4096; // + unmapped guard gap
        for (size_t p = 0; p < stack_pages; p++) {
            uint64_t ph = pmm::alloc_zero();
            if (!ph) return false;
            if (!paging::map(vbase + p * 4096, ph, paging::F_PRESENT | paging::F_RW)) {
                pmm::free(ph);
                return false;
            }
        }
        uint64_t top = vbase + stack_pages * 4096;
        s_threads[i].kstack_top = top;
        uint64_t *sp = (uint64_t *)(top - 256); // headroom for an IRQ frame at rsp0
        *--sp = (uint64_t)thread_trampoline; // ret addr
        *--sp = 0;                           // rbx
        *--sp = 0;                           // rbp
        *--sp = (uint64_t)entry;             // r12 = func
        *--sp = (uint64_t)arg;               // r13 = arg
        *--sp = 0;                           // r14
        *--sp = 0;                           // r15
        s_threads[i].rsp = (uint64_t)sp;
        s_threads[i].pid = ++s_next_pid;
        s_threads[i].pgid = s_threads[i].pid;
        s_threads[i].parent_pid = s_current ? s_current->pid : 1;
        s_threads[i].name = name;
        s_threads[i].state = ST_RUN;
        s_threads[i].runs = 0;
        s_threads[i].stack_pages = stack_pages;
        s_threads[i].syscall_stack = alloc_syscall_stack();
        s_threads[i].cr3 = s_kernel_cr3 ? s_kernel_cr3 : paging::current_pml4_phys();
        s_count++;
        console::kprintf(FG, BG, "[sched] spawned '%s' stack=%p pages=%u\n",
                         name, (void *)vbase, (unsigned)stack_pages);
        return true;
    }
    return false;
}

// User foreground thread: runs on a private address space. The kernel half
// is shared, so the bootstrap's kernel stack stays valid after the switch.
// NOTE: kernel stacks for user threads live at 0xFFFFFFFFC0000000, well
// clear of the heap (0xFFFFFFFFA0000000+64MB) and of spawn()'s stacks
// (0xFFFFFFFFB0000000): an earlier build aliased the heap base and every
// spawn overwrote 4 heap PTEs, "wiping" the initrd copy (same vaddr,
// different frames).
bool spawn_user(const char *name, Entry entry, void *arg, uint64_t user_cr3,
                size_t stack_pages, uint32_t *pid_out, bool owns_cr3) {
    if (!user_cr3) return false;
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (s_threads[i].state != ST_FREE) continue;
        static uint64_t s_uvirt = 0xFFFFFFFFC0000000ULL;
        uint64_t vbase = s_uvirt;
        s_uvirt += stack_pages * 4096 + 4096;
        for (size_t p = 0; p < stack_pages; p++) {
            uint64_t ph = pmm::alloc_zero();
            if (!ph) return false;
            if (!paging::map(vbase + p * 4096, ph, paging::F_PRESENT | paging::F_RW)) {
                pmm::free(ph);
                return false;
            }
        }
        uint64_t top = vbase + stack_pages * 4096;
        s_threads[i].kstack_top = top;
        uint64_t *sp = (uint64_t *)(top - 256); // headroom for an IRQ frame at rsp0
        *--sp = (uint64_t)thread_trampoline;
        *--sp = 0;
        *--sp = 0;
        *--sp = (uint64_t)entry;
        *--sp = (uint64_t)arg;
        *--sp = 0;
        *--sp = 0;
        s_threads[i].rsp = (uint64_t)sp;
        s_threads[i].pid = ++s_next_pid;
        s_threads[i].pgid = s_threads[i].pid;
        s_threads[i].parent_pid = s_current ? s_current->pid : 1;
        s_threads[i].name = name;
        s_threads[i].state = ST_RUN;
        s_threads[i].runs = 0;
        s_threads[i].stack_pages = stack_pages;
        s_threads[i].syscall_stack = alloc_syscall_stack();
        s_threads[i].cr3 = user_cr3;
        s_threads[i].owns_cr3 = owns_cr3;
        if (pid_out) *pid_out = s_threads[i].pid;
        s_count++;
        return true;
    }
    return false;
}

void yield() {
    if (!s_started || s_count < 2) return;
    cpu::cli();
    do_switch();
    cpu::sti();
}

void tick() {
    if (s_hook) s_hook();
    // Charge this tick to whoever the timer interrupted. Before the quantum
    // check: a single runnable thread is still using the CPU.
    if (s_started && s_current) s_current->cpu_ticks++;
    if (!s_started || s_count < 2) return;
    uint64_t now = pit::ticks();
    if (now - s_last_tick < QUANTUM_TICKS) return;
    s_last_tick = now;
    do_switch(); // interrupts already off (interrupt gate), iret restores them
}

void sleep_ms(uint64_t ms) {
    // Round up so sleeps of < (1000/HZ) ms still wait at least one tick;
    // otherwise a tight polling loop with sleep_ms(1) at 100 Hz spins forever.
    uint64_t want = ms * pit::HZ / 1000;
    if (ms && !want) want = 1;
    uint64_t target = pit::ticks() + want;
    while ((int64_t)(pit::ticks() - target) < 0) yield();
}

void exit() {
    cpu::cli();
    s_current->state = ST_DEAD;
    if (s_count > 0) s_count--;
    console::kprintf(FG, BG, "[sched] '%s' exited\n", s_current->name);
    if (s_current == &s_threads[0] || s_count == 0) {
        for (;;) { asm volatile("hlt"); }
    }
    do_switch();
    for (;;) { asm volatile("hlt"); } // unreachable
}

const char *current_name() { return s_current ? s_current->name : "?"; }
void *current_handle() { return (void *)s_current; }
void reap_pid(uint32_t pid) {
    for (auto &t : s_threads)
        if (t.state == ST_DEAD && t.pid == pid) { t = Thread{}; return; }
}
uint32_t current_pid() { return s_current ? s_current->pid : 0; }
uint32_t current_pgid() { return s_current ? s_current->pgid : 0; }
bool set_pgid(uint32_t pid, uint32_t pgid) {
    for (auto &t : s_threads)
        if (t.state != ST_FREE && t.pid == pid) { t.pgid = pgid; return true; }
    return false;
}
uint64_t current_cr3() { return s_current ? s_current->cr3 : 0; }
void set_current_cr3(uint64_t root) {
    if (!s_current) return;
    s_current->cr3 = root;
    paging::write_cr3(root);
}
uint32_t pid_of(void *h) {
    auto *t = (Thread *)h;
    return (t && t->state != ST_FREE) ? t->pid : 0;
}
void set_exit_status(long status) {
    if (s_current) s_current->exit_status = status;
}
long wait_child(int64_t wanted, uint64_t status_ptr) {
    uint32_t parent = current_pid();
    for (;;) {
        bool any = false;
        for (size_t i = 0; i < MAX_THREADS; i++) {
            Thread &t = s_threads[i];
            if (t.state == ST_FREE || t.parent_pid != parent) continue;
            if (wanted > 0 && t.pid != (uint32_t)wanted) continue;
            any = true;
            if (t.state != ST_DEAD) continue;
            if (status_ptr) *(uint32_t *)status_ptr = (uint32_t)(t.exit_status << 8);
            uint32_t pid = t.pid;
            if (t.owns_cr3) paging::destroy_user_root(t.cr3);
            t = Thread{};
            return (long)pid;
        }
        if (!any) return -10; // ECHILD
        // Syscalls use a single legacy entry stack. Do not yield while a
        // caller's syscall frame is live; userspace retries after yielding.
        return -11; // EAGAIN
    }
}

bool kill(void *h) {
    auto *t = (Thread *)h;
    if (!t || t->state != ST_RUN) return false;
    if (t == s_current) { // suicide from inside a handler == exit path
        exit();
    }
    t->state = ST_DEAD;
    if (s_count > 0) s_count--;
    return true;
}

bool signal_pid(uint32_t pid, uint32_t sig) {
    for (size_t i = 0; i < MAX_THREADS; i++) {
        Thread &t = s_threads[i];
        if (t.state != ST_RUN || t.pid != pid) continue;
        t.exit_status = 128 + (long)sig;
        t.state = ST_DEAD;
        if (s_count > 0) s_count--;
        return true;
    }
    return false;
}

unsigned signal_group(uint32_t pgid, uint32_t sig) {
    unsigned n = 0;
    for (auto &t : s_threads) {
        if (t.state != ST_RUN || t.pgid != pgid) continue;
        t.exit_status = 128 + (long)sig;
        t.state = ST_DEAD;
        if (s_count > 0) s_count--;
        n++;
    }
    return n;
}

void set_tick_hook(TickHook fn) { s_hook = fn; }
uint64_t thread_count() { return s_count; }
uint64_t switches() { return s_switches; }

void dump() {
    console::println("-- threads --", ACC);
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (s_threads[i].state == ST_FREE) continue;
        console::kprintf(FG, BG, "  [%u] %s runs=%llu%s%s\n", (unsigned)i, s_threads[i].name, s_threads[i].runs,
                         (&s_threads[i] == s_current) ? " <-" : "",
                         s_threads[i].state == ST_DEAD ? " (exited)" : "");
    }
}

bool stat(size_t i, const char **name_out, uint64_t *runs_out, bool *cur_out) {
    size_t seen = 0;
    for (size_t k = 0; k < MAX_THREADS; k++) {
        if (s_threads[k].state == ST_FREE) continue;
        if (seen++ != i) continue;
        if (name_out) *name_out = s_threads[k].name;
        if (runs_out) *runs_out = s_threads[k].runs;
        if (cur_out) *cur_out = (&s_threads[k] == s_current);
        return true;
    }
    return false;
}

bool info(size_t i, ThreadInfo &out) {
    size_t seen = 0;
    for (size_t k = 0; k < MAX_THREADS; k++) {
        if (s_threads[k].state == ST_FREE) continue;
        if (seen++ != i) continue;
        const Thread &t = s_threads[k];
        out.pid = t.pid;
        out.ppid = t.parent_pid;
        out.name = t.name;
        // 'R' or 'Z' only: ChocoOS has no separate blocked/stopped state yet, so
        // claiming more would be invented data (see ROADMAP phase 21).
        out.state = t.state == ST_DEAD ? 'Z' : 'R';
        out.runs = t.runs;
        out.cpu_ticks = t.cpu_ticks;
        out.stack_bytes = t.stack_pages * 4096;
        // owns_cr3 only says who frees the PML4; membership is the space itself.
        out.user = t.cr3 != 0 && t.cr3 != s_kernel_cr3;
        out.idle = t.idle_task;
        out.current = (&t == s_current);
        return true;
    }
    return false;
}

} // namespace choco::sched

extern "C" void choco_thread_exit() { choco::sched::exit(); }

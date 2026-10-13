#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::gdt {

// Long-mode GDT: kernel code/data, user code/data (for phase 5),
// plus a 64-bit TSS (rsp0 + IST1 for double fault).
void init();
uint16_t code_selector();
uint16_t tss_selector();
// Set the interrupt stack (rsp0); the scheduler points it at the running
// thread's kernel stack on every context switch. 0 = boot default.
void set_rsp0(uint64_t top);

} // namespace choco::gdt

namespace choco::sched {

using Entry = void (*)(void *);

// Must be called with interrupts disabled, after gdt::init().
// The boot path becomes the "idle" thread.
void init();
void set_started(bool on);

// 4 pages (16 KiB) stacks from the PMM. Returns false when full.
bool spawn(const char *name, Entry entry, void *arg, size_t stack_pages = 4);
// User thread on a private address space (kernel stack shared, CR3=user_cr3).
bool spawn_user(const char *name, Entry entry, void *arg, uint64_t user_cr3,
                size_t stack_pages = 4, uint32_t *pid_out = nullptr,
                bool owns_cr3 = false);

void yield();       // voluntary switch (thread context)
void tick();        // called from the timer IRQ (interrupts already off)
void sleep_ms(uint64_t ms); // cooperative sleep (yields until the deadline)
[[noreturn]] void exit(); // park the current thread forever

void *current_handle(); // opaque Thread* of the running thread (for proc exit/kill)
uint32_t current_pid();
uint32_t current_pgid();
bool set_pgid(uint32_t pid, uint32_t pgid);
uint64_t current_cr3();
void set_current_cr3(uint64_t root);
uint32_t pid_of(void *h);
void set_exit_status(long status);
long wait_child(int64_t pid, uint64_t status_ptr);
// Free a DEAD thread's slot (e.g. the foreground job, which nobody wait4()s).
// Does not touch its address space or exit status.
void reap_pid(uint32_t pid);
bool kill(void *h);     // remove a thread from rotation (stacks leak by design, phase 5)
bool signal_pid(uint32_t pid, uint32_t sig); // terminate a non-current Linux task
unsigned signal_group(uint32_t pgid, uint32_t sig);

using TickHook = void (*)(); // runs inside every timer tick (interrupts off)
void set_tick_hook(TickHook fn);

const char *current_name();
uint64_t thread_count();
uint64_t switches();
void dump();
// Introspection for /proc/threads (index over live slots, order unstable).
bool stat(size_t i, const char **name_out, uint64_t *runs_out, bool *cur_out);

// Everything a process table needs, for /proc/processes and /proc/<pid>/stat.
struct ThreadInfo {
    uint32_t pid = 0;
    uint32_t ppid = 0;
    const char *name = "";
    char state = 'R';       // 'R' running/runnable, 'Z' exited
    uint64_t runs = 0;      // times scheduled
    uint64_t cpu_ticks = 0; // 1 tick = 10 ms (pit::HZ == 100)
    uint64_t stack_bytes = 0;
    bool user = false;      // owns a ring-3 address space
    bool idle = false;      // the idle task: its ticks are not busy time
    bool current = false;
};
bool info(size_t i, ThreadInfo &out);

} // namespace choco::sched

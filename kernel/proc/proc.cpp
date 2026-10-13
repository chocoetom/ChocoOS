// Foreground user processes, phase 7: each program runs on a private
// address space (low half), with argc/argv on its stack and a brk heap.
// Only one foreground task at a time; its pages are freed on exit/kill.
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/gui.hpp>
#include <choco/mem.hpp>
#include <choco/sched.hpp>
#include <choco/tty.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;
using choco::console::ERR;

extern "C" void user_enter(uint64_t entry, uint64_t stack_top);
extern "C" void user_resume(uint64_t entry, uint64_t stack_top);

namespace choco::proc {
namespace {

constexpr uint64_t USTACK_TOP = 0x700000ULL;
constexpr size_t USTACK_PAGES = 16; // 64 KiB: musl programs want headroom
constexpr size_t ARGV_MAX = 8;
constexpr size_t LINUX_SIG_N = 32;

struct LinuxSigAction {
    uint64_t handler = 0;
    uint64_t flags = 0;
    uint64_t restorer = 0;
    uint64_t mask = 0;
};
struct LinuxSignalFrame {
    uint64_t magic;
    cpu::Frame saved;
    uint64_t oldmask;
};
constexpr uint64_t LINUX_SIGFRAME_MAGIC = 0x43484F434F534947ULL;

// One user process: its private address space, heap, user stack, name and
// signal state. There is no single global foreground any more — a process is
// looked up by pid, so a forked child's exec()/syscalls no longer clobber the
// parent's state (the old `s_fg` limit).
struct Proc {
    bool used = false;
    bool owns_root = true;   // false when a CLONE_VM thread shares a root
    // A detached job: launched by the WM (desktop/taskbar/menu), not by the
    // console `run`. Nobody waits on it, so the ambient loop reaps it with
    // reap_jobs() once it is done, and several can run at the same time.
    bool job = false;
    void *thread = nullptr;  // the user thread handle once it starts
    volatile bool done = false;
    volatile long code = -1;
    volatile bool kill_me = false;
    uint64_t root = 0;       // private PML4
    uint64_t brk = 0;        // current break (sbrk syscall backend)
    uint64_t brk_start = 0;  // break right after loading: brk - this is heap growth
    char comm[32]{};         // program name (basename of the path), for /proc
    uint64_t stack_base = 0; // low virt of the user stack
    uint64_t stack_phys[USTACK_PAGES]{};
    uint64_t aux_phdr = 0, aux_phent = 0, aux_phnum = 0, aux_entry = 0, aux_interp = 0;
    uint32_t pid = 0, ppid = 1;
    LinuxSigAction sig[LINUX_SIG_N]{};
    uint64_t sigmask = 0;
    uint64_t sigpending = 0; // deferred signals waiting for a handler (ISIG, SIGWINCH)
};
constexpr size_t PROC_N = 16;
Proc s_procs[PROC_N]{};
uint32_t s_fg_pid = 0;       // pid of the console foreground job (0 = none)

Proc *proc_of(uint32_t pid) {
    for (auto &p : s_procs)
        if (p.used && p.pid == pid) return &p;
    return nullptr;
}
// The calling thread's process (null for kernel threads like the shell/idle).
Proc *cur() { return proc_of(sched::current_pid()); }
// The console foreground job (null when the shell is idle at the prompt).
Proc *fg() { return s_fg_pid ? proc_of(s_fg_pid) : nullptr; }

Proc *proc_alloc(uint32_t pid) {
    for (auto &p : s_procs)
        if (!p.used) {
            util::memset(&p, 0, sizeof(Proc)); // in place: no big stack temp
            p.used = true;
            p.pid = pid;
            p.ppid = 1;
            return &p;
        }
    return nullptr;
}

struct Bootstrap {
    uint64_t entry;
    uint64_t stack_top;
};
struct CloneStart { uint64_t entry, stack; };

// Write through the HHDM into a process's address space, which is not in CR3.
void write_as(Proc &p, uint64_t virt, const void *src, size_t n) {
    uint64_t hhdm = paging::hhdm();
    size_t off = 0;
    while (off < n) {
        uint64_t v = virt + off;
        uint64_t page_off = v & 0xFFFULL;
        size_t chunk = (size_t)(4096 - page_off);
        if (chunk > n - off) chunk = n - off;
        uint64_t ph = paging::translate_as(p.root, v & ~0xFFFULL);
        if (!ph) return;
        util::memcpy((void *)(ph + hhdm + page_off),
                     (const uint8_t *)src + off, chunk);
        off += chunk;
    }
}

void write_u64(Proc &p, uint64_t virt, uint64_t v) { write_as(p, virt, &v, 8); }

void bootstrap(void *arg) {
    auto *b = (Bootstrap *)arg;
    Proc *p = cur();
    if (p) p->thread = sched::current_handle();
    uint64_t entry = b->entry, top = b->stack_top;
    heap::free(b);
    user_enter(entry, top);
    for (;;) { asm volatile("hlt"); } // unreachable
}

void clone_bootstrap(void *arg) {
    auto *c = (CloneStart *)arg;
    Proc *p = cur();
    if (p) p->thread = sched::current_handle();
    uint64_t entry = c->entry, stack = c->stack;
    heap::free(c);
    user_resume(entry, stack);
    for (;;) asm volatile("hlt");
}

// Keyboard input is handled by the run-loop pump, which feeds the tty
// discipline; the discipline raises SIGINT/SIGQUIT itself, so no periodic
// tick hook is needed for the foreground job.

bool map_ustack(Proc &p) {
    p.stack_base = USTACK_TOP - USTACK_PAGES * 4096;
    for (size_t i = 0; i < USTACK_PAGES; i++) {
        uint64_t v = p.stack_base + i * 4096;
        uint64_t ph = pmm::alloc_zero();
        if (!ph) return false;
        p.stack_phys[i] = ph;
        if (!paging::map_as(p.root, v, ph,
                             paging::F_PRESENT | paging::F_RW | paging::F_USER))
            return false;
    }
    return true;
}

// Pushes argc/argv onto the user stack. Returns the new stack top, 0 on
// overflow. Layout (descending): strings, then argv[] (+NULL), then argc.
// The array goes strictly BELOW the lowest string: an earlier build placed
// it overlapping the last string, so the NULL terminator erased argv[1].
uint64_t push_args(Proc &p, int argc, char **argv) {
    if (argc < 1) argc = 1;
    if (argc > (int)ARGV_MAX) argc = (int)ARGV_MAX;
    uint64_t sp = USTACK_TOP;
    uint64_t ptrs[ARGV_MAX]{};
    for (int i = 0; i < argc; i++) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        size_t n = util::strlen(s) + 1;
        if (n > 512) n = 512;
        sp -= (n + 7) & ~7ULL;
        if (sp < p.stack_base + 64) return 0;
        char tmp[512]{};
        for (size_t k = 0; k < n - 1 && s[k]; k++) tmp[k] = s[k];
        write_as(p, sp, tmp, n);
        ptrs[i] = sp;
    }
    // 8 bytes for argc + (argc+1) pointers, 16-byte aligned, all below sp.
    // Linux layout after that: envp NULL, then auxv pairs + AT_NULL.
    // Native _start ignores everything past argv; Linux _start needs it.
    uint64_t rnd_ptr = 0, fn_ptr = 0;
    {
        // 16 random bytes (PIT-seeded, not crypto) + exec path copy.
        uint64_t s = pit::ticks() * 6364136223846793005ULL + 1442695040888963407ULL;
        char rnd[16]{};
            for (int i = 0; i < 16; i++) {
                s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                rnd[i] = (char)(s >> 33);
            }
            // glibc derives the stack and pointer guards from different
            // halves of AT_RANDOM. Keep them identical until the loader
            // exposes separate guard initialization to the ABI.
        sp -= 16;
        if (sp < p.stack_base + 64) return 0;
        write_as(p, sp, rnd, 16);
        rnd_ptr = sp;
        const char *fn = (argv && argc > 0 && argv[0]) ? argv[0] : "";
        size_t n = util::strlen(fn) + 1;
        if (n > 128) n = 128;
        sp -= (n + 7) & ~7ULL;
        if (sp < p.stack_base + 64) return 0;
        char fntmp[128]{};
        for (size_t k = 0; k < n - 1 && fn[k]; k++) fntmp[k] = fn[k];
        write_as(p, sp, fntmp, n);
        fn_ptr = sp;
    }
    constexpr uint64_t NAUX = 14; // 13 pairs + AT_NULL
    sp = (sp - 8 - (uint64_t)(argc + 1) * 8 - 8 - NAUX * 16) & ~15ULL;
    if (sp < p.stack_base + 64) return 0;
    for (int i = 0; i < argc; i++) write_u64(p, sp + 8 + (uint64_t)i * 8, ptrs[i]);
    write_u64(p, sp + 8 + (uint64_t)argc * 8, 0);
    write_u64(p, sp, (uint64_t)argc);
    // envp: empty (single NULL)
    uint64_t envp = sp + 8 + (uint64_t)(argc + 1) * 8;
    write_u64(p, envp, 0);
    // auxv pairs
    uint64_t ap = envp + 8;
    auto aux = [&](uint64_t tag, uint64_t val) {
        write_u64(p, ap, tag);
        write_u64(p, ap + 8, val);
        ap += 16;
    };
    aux(3, p.aux_phdr);   // AT_PHDR
    aux(4, p.aux_phent);  // AT_PHENT
    aux(5, p.aux_phnum);  // AT_PHNUM
    aux(9, p.aux_entry);  // AT_ENTRY
    aux(11, 0);              // AT_UID
    aux(12, 0);              // AT_EUID
    aux(13, 0);              // AT_GID
    aux(14, 0);              // AT_EGID
    aux(17, 100);            // AT_CLKTCK
    aux(6, 4096);            // AT_PAGESZ
    aux(8, 0);               // AT_FLAGS
    aux(25, rnd_ptr);        // AT_RANDOM
    aux(31, fn_ptr);         // AT_EXECFN
    aux(0, 0);               // AT_NULL
    return sp;
}

// Tear down a process: reap its display-server windows, drop its address space
// and release the slot. The PML4 is freed only when the process owns it (a
// CLONE_VM thread shares the parent's, freed when the parent goes).
void cleanup(Proc &p) {
    if (p.root) {
        // Reap display-server windows first (unmaps + frees their frames),
        // then drop the whole address space. Order matters: destroy would
        // otherwise free the same frames (pmm tolerates it, but be clean).
        gui::reap_user(p.root);
        // Back on the kernel root before freeing the user tables.
        paging::write_cr3(paging::current_pml4_phys());
        if (p.owns_root) paging::destroy_user_root(p.root);
        p.root = 0;
    }
    p.used = false;
    p.thread = nullptr;
}

// The basename of the program path, the way Linux fills /proc/PID/comm.
void set_comm(Proc &p, const char *path) {
    const char *base = path;
    for (const char *q = path; *q; q++)
        if (*q == '/') base = q + 1;
    size_t n = 0;
    for (; base[n] && n + 1 < sizeof(p.comm); n++) p.comm[n] = base[n];
    p.comm[n] = 0;
}

} // namespace

// Accessors report the CALLING thread's process (null for kernel threads).
bool foreground_active() { Proc *p = fg(); return p && p->thread && !p->done; }
uint32_t foreground_pid() { return s_fg_pid; }
uint64_t proc_brk() { Proc *p = cur(); return p ? p->brk : 0; }
// Heap the program asked for beyond what loading it needed: growth, not a claim
// about pages still mapped.
uint64_t proc_data_bytes() {
    Proc *p = cur();
    if (!p || !p->thread || p->done || p->brk < p->brk_start) return 0;
    return p->brk - p->brk_start;
}
uint64_t foreground_data_bytes() {
    Proc *p = fg();
    if (!p || p->brk < p->brk_start) return 0;
    return p->brk - p->brk_start;
}
const char *proc_comm() { Proc *p = cur(); return p ? p->comm : ""; }
const char *foreground_comm() { Proc *p = fg(); return p ? p->comm : ""; }
uint64_t proc_root() { Proc *p = cur(); return p ? p->root : 0; }
uint32_t proc_pid() { Proc *p = cur(); return p ? p->pid : 1; }
uint32_t proc_ppid() { Proc *p = cur(); return p ? p->ppid : 1; }

namespace {
bool copy_user_string(uint64_t ptr, char *dst, size_t cap) {
    if (!ptr || ptr >= 0x0000800000000000ULL || cap == 0) return false;
    for (size_t i = 0; i + 1 < cap; i++) {
        uint64_t addr = ptr + i;
        if (addr < ptr || !paging::translate(addr & ~0xFFFULL)) return false;
        char c = *(const char *)addr;
        dst[i] = c;
        if (!c) return true;
    }
    dst[cap - 1] = 0;
    return false;
}
}

long exec_current(uint64_t path_ptr, uint64_t argv_ptr, void *frame_ptr) {
    Proc *p = cur();
    if (!p || !p->root) return -8; // no process / no address space
    char path[256]{};
    if (!copy_user_string(path_ptr, path, sizeof(path))) return -14;
    const void *data = nullptr;
    size_t size = 0;
    if (!fs::read(path, &data, &size)) return -2;

    char argbuf[ARGV_MAX][128]{};
    char *av[ARGV_MAX]{};
    int ac = 0;
    av[ac++] = path;
    if (argv_ptr && argv_ptr < 0x0000800000000000ULL) {
        for (int i = 0; i < (int)ARGV_MAX; i++) {
            uint64_t slot = argv_ptr + (uint64_t)i * 8;
            if (slot < argv_ptr || !paging::translate(slot & ~0xFFFULL)) break;
            uint64_t q = *(const uint64_t *)slot;
            if (!q) break;
            if (!copy_user_string(q, argbuf[i], sizeof(argbuf[0]))) return -14;
            av[i] = argbuf[i];
            ac = i + 1;
        }
    }

    // Build the new image first, so a failure leaves the process running the old
    // program (exec returns -ENOMEM and the caller carries on).
    uint64_t old_root = p->root;
    bool old_owns = p->owns_root;
    uint64_t old_brk = p->brk, old_brk_start = p->brk_start;
    uint64_t new_root = paging::create_user_root();
    if (!new_root) return -12;
    uint64_t brk = 0;
    elf::Loaded img = elf::load_as(new_root, data, size, brk);
    if (!img.ok) {
        paging::write_cr3(paging::current_pml4_phys());
        paging::destroy_user_root(new_root);
        return -8;
    }

    // Replace THIS process's state in place (same pid). There is no global
    // foreground to stash: a forked child's exec only rewrites the child.
    p->root = new_root;
    p->owns_root = true;
    p->brk = brk;
    p->brk_start = brk;
    p->thread = sched::current_handle();
    p->done = false;
    p->kill_me = false;
    p->code = -1;
    p->stack_base = 0;
    for (size_t i = 0; i < USTACK_PAGES; i++) p->stack_phys[i] = 0;
    p->aux_phdr = img.phdr_vaddr;
    p->aux_phent = img.phent;
    p->aux_phnum = img.phnum;
    p->aux_entry = img.entry;
    p->aux_interp = 0;
    if (img.interp_path) {
        // PT_INTERP: copy the interpreter path just above the break, page-aligned.
        const char *interp = img.interp_path;
        size_t interp_len = util::strlen(interp) + 1;
        uint64_t interp_page = (brk + 0x1000) & ~0xFFFULL;
        uint64_t hhdm = paging::hhdm();
        size_t off = 0;
        while (off < interp_len) {
            size_t chunk = 4096 - (interp_page & 0xFFFULL);
            if (chunk > interp_len - off) chunk = interp_len - off;
            uint64_t ph = paging::translate_as(new_root, interp_page & ~0xFFFULL);
            uint8_t *dst = (uint8_t *)(ph + hhdm + (interp_page & 0xFFFULL));
            util::memcpy(dst, interp + off, chunk);
            off += chunk;
            interp_page += chunk;
        }
        p->aux_interp = interp_page - interp_len;
    }
    if (!map_ustack(*p)) {
        p->root = old_root; p->owns_root = old_owns;
        p->brk = old_brk; p->brk_start = old_brk_start;
        paging::write_cr3(paging::current_pml4_phys());
        paging::destroy_user_root(new_root);
        return -12;
    }
    uint64_t top = push_args(*p, ac, av);
    if (!top) {
        p->root = old_root; p->owns_root = old_owns;
        p->brk = old_brk; p->brk_start = old_brk_start;
        paging::write_cr3(paging::current_pml4_phys());
        paging::destroy_user_root(new_root);
        return -12;
    }
    set_comm(*p, path);

    auto *f = (cpu::Frame *)frame_ptr;
    f->rip = img.entry;
    f->rsp = top;
    f->rax = 0;
    // Drop the old space (and its windows) off the active root, then run on the
    // new one. A CLONE_VM thread's old root is shared, so it is not destroyed.
    paging::write_cr3(paging::current_pml4_phys());
    gui::reap_user(old_root);
    if (old_owns) paging::destroy_user_root(old_root);
    sched::set_current_cr3(new_root);
    linux::reset();
    linux::set_fs(0);
    return 0;
}

long clone_current(uint64_t flags, uint64_t child_stack, void *frame_ptr) {
    constexpr uint64_t CLONE_VM = 0x00000100ULL;
    Proc *parent = cur();
    if (!parent || !parent->root) return -38;
    auto *f = (cpu::Frame *)frame_ptr;
    auto *c = (CloneStart *)heap::alloc(sizeof(CloneStart));
    if (!c) return -12;
    c->entry = f->rip;
    c->stack = child_stack ? child_stack : f->rsp;
    const bool share_vm = (flags & CLONE_VM) != 0;
    uint64_t child_root = share_vm ? parent->root : paging::clone_user_root(parent->root);
    if (!child_root) {
        heap::free(c);
        return -12;
    }
    uint32_t pid = 0;
    if (!sched::spawn_user("clone", clone_bootstrap, c, child_root, 4, &pid, !share_vm)) {
        if (!share_vm) paging::destroy_user_root(child_root);
        heap::free(c);
        return -11;
    }
    // Give the child its own process entry (state copied from the parent), so
    // its syscalls and any exec() no longer clobber the parent.
    Proc *child = proc_alloc(pid);
    if (child) {
        child->root = child_root;
        child->owns_root = !share_vm;
        child->brk = parent->brk;
        child->brk_start = parent->brk_start;
        util::memcpy(child->comm, parent->comm, sizeof(child->comm));
        util::memcpy(child->sig, parent->sig, sizeof(child->sig));
        child->sigmask = parent->sigmask;
        child->ppid = parent->pid;
    }
    syscall::clone_files(pid);
    sched::set_pgid(pid, sched::current_pgid());
    return (long)pid;
}

uint64_t proc_sbrk(int64_t incr) {
    Proc *p = cur();
    if (!p || !p->root) return (uint64_t)-1;
    if (incr == 0) return p->brk;
    if (incr < 0) return (uint64_t)-1; // no shrink yet
    uint64_t old = p->brk;
    uint64_t want = old + (uint64_t)incr;
    if (want < old || want >= 0x0000800000000000ULL) return (uint64_t)-1;
    if (want >= USTACK_TOP - USTACK_PAGES * 4096 - 65536) return (uint64_t)-1;
    uint64_t base = old & ~0xFFFULL;
    uint64_t end = (want + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t v = base; v < end; v += 4096) {
        if (paging::translate_as(p->root, v)) continue;
        uint64_t ph = pmm::alloc_zero();
        if (!ph) return (uint64_t)-1;
        if (!paging::map_as(p->root, v, ph,
                             paging::F_PRESENT | paging::F_RW | paging::F_USER)) {
            pmm::free(ph);
            return (uint64_t)-1;
        }
    }
    p->brk = want;
    return old;
}

void exit_current(long code) {
    Proc *p = cur();
    if (p) p->code = code;
    sched::set_exit_status(code);
    if (p && p->job) {
        // A detached job: no one is waiting on `done`. Mark it under cli so the
        // ambient loop cannot reap (and free the address space) between the flag
        // and the context switch that takes this thread off that space.
        cpu::cli();
        p->done = true;
        sched::exit(); // never returns
    }
    if (p && p->pid == s_fg_pid) {
        // The console foreground job: the shell thread is waiting on `done` and
        // will clean the address space up. Park here until it does.
        syscall::reset_files();
        p->done = true;
        sched::exit(); // never returns
    }
    // A forked/cloned child: drop its per-process IO, free its slot and let the
    // parent reap it (wait4 frees the address space via owns_cr3).
    syscall::drop_files(sched::current_pid());
    if (p) p->used = false;
    sched::exit(); // never returns
}

long linux_sigaction(uint64_t sig, uint64_t act, uint64_t oldact) {
    if (sig == 0 || sig >= LINUX_SIG_N || sig == 9 || sig == 19) return -22;
    Proc *p = cur();
    if (!p) return -9;
    if (oldact && !syscall::user_ok(oldact, sizeof(LinuxSigAction))) return -14;
    if (act && !syscall::user_ok(act, sizeof(LinuxSigAction))) return -14;
    if (oldact) util::memcpy((void *)oldact, &p->sig[sig], sizeof(LinuxSigAction));
    if (act) util::memcpy(&p->sig[sig], (const void *)act, sizeof(LinuxSigAction));
    return 0;
}

long linux_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset) {
    if (oldset && !syscall::user_ok(oldset, 8)) return -14;
    if (set && !syscall::user_ok(set, 8)) return -14;
    Proc *p = cur();
    if (!p) return -9;
    if (oldset) *(uint64_t *)oldset = p->sigmask;
    if (!set) return 0;
    uint64_t value = *(const uint64_t *)set;
    value &= ~((1ULL << 9) | (1ULL << 19)); // SIGKILL/SIGSTOP cannot be masked
    if (how == 0) p->sigmask |= value;
    else if (how == 1) p->sigmask &= ~value;
    else if (how == 2) p->sigmask = value;
    else return -22;
    return 0;
}

bool linux_signal_ignored(uint64_t sig) {
    Proc *p = fg();
    return p && sig < LINUX_SIG_N && p->sig[sig].handler == 1;
}

bool linux_deliver_signal(uint64_t sig, void *frame_ptr) {
    Proc *p = cur();
    if (!p) return false;
    if (sig == 9 || sig == 19 || sig >= LINUX_SIG_N) return false;
    if (p->sigmask & (1ULL << sig)) return false; // blocked
    uint64_t handler = p->sig[sig].handler;
    if (!handler || handler == 1) return false;
    auto *f = (cpu::Frame *)frame_ptr;
    LinuxSignalFrame sf{};
    sf.magic = LINUX_SIGFRAME_MAGIC;
    sf.saved = *f;
    sf.oldmask = p->sigmask;
    uint64_t new_rsp = (f->rsp - sizeof(sf) - 15) & ~15ULL;
    if (!syscall::user_ok(new_rsp, sizeof(sf))) return false;
    write_as(*p, new_rsp, &sf, sizeof(sf));
    p->sigmask |= (1ULL << sig) | p->sig[sig].mask;
    // The handler receives the signal number in rdi and returns through the
    // Linux rt_sigreturn syscall, which restores the saved frame below.
    f->rip = handler;
    f->rsp = new_rsp;
    f->rdi = sig;
    f->rsi = 0;
    f->rdx = 0;
    return true;
}

long linux_sigreturn(void *frame_ptr) {
    auto *f = (cpu::Frame *)frame_ptr;
    uint64_t rsp = f->rsp;
    if (!syscall::user_ok(rsp, sizeof(LinuxSignalFrame))) return -14;
    const auto *sf = (const LinuxSignalFrame *)rsp;
    if (sf->magic != LINUX_SIGFRAME_MAGIC) return -22;
    *f = sf->saved;
    Proc *p = cur();
    if (p) p->sigmask = sf->oldmask;
    return (long)f->rax;
}

// Signal raised by the tty discipline (ISIG) or the console (SIGWINCH). With a
// handler installed the signal is deferred to the syscall return path; a
// default-fatal signal terminates the job exactly as a real ISIG would.
void raise_signal(uint32_t sig) {
    Proc *p = fg();
    if (!p || !foreground_active() || sig == 0 || sig >= LINUX_SIG_N) return;
    if (linux_signal_ignored(sig)) return;
    if (p->sig[sig].handler && p->sig[sig].handler != 1) {
        p->sigpending |= (1ULL << sig);
        return;
    }
    if (sig == 2 || sig == 3 || sig == 15) { // SIGINT/SIGQUIT/SIGTERM: default fatal
        console::println(sig == 2 ? "^C (killed)" : "^\\ (killed)", ERR);
        p->kill_me = true;
        sched::signal_group(p->pid, sig);
        p->code = 128 + (long)sig;
        p->done = true;
    }
    // SIGWINCH/SIGCHLD and other default-ignore signals are dropped.
}

// Force the foreground program to terminate from outside (the WM close button).
// Unlike raise_signal a user handler cannot defer it, so closing the window
// always tears the job down.
void kill_foreground() {
    Proc *p = fg();
    if (!p || !foreground_active()) return;
    p->kill_me = true;
    sched::signal_group(p->pid, 15); // SIGTERM
    p->code = 128 + 15;
    p->done = true;
}

bool has_pending() { Proc *p = cur(); return p && p->sigpending != 0; }

// Called on the syscall return path: hand deferred signals to their handlers.
// linux_deliver_signal rewrites the frame's rip/rsp, so the next iretq lands
// in the user handler.
void deliver_pending(void *frame_ptr) {
    Proc *p = cur();
    if (!p || !p->sigpending) return;
    for (uint64_t sig = 1; sig < LINUX_SIG_N; sig++) {
        uint64_t bit = 1ULL << sig;
        if (!(p->sigpending & bit)) continue;
        p->sigpending &= ~bit;
        linux_deliver_signal(sig, frame_ptr);
    }
}

// Read + load a program into a fresh Proc with its user stack and argv on it.
// Shared by the blocking console `run` and the non-blocking WM spawn_job; on
// success it returns the Proc and a Bootstrap ready for the thread entry, on
// failure it releases the slot and returns null.
Proc *load_proc(const char *path, int argc, char **argv, Bootstrap **boot_out) {
    const void *data = nullptr;
    size_t size = 0;
    if (!fs::read(path, &data, &size)) {
        console::kprintf(ERR, BG, "run: '%s' not found in initrd (try 'ls')\n", path);
        return nullptr;
    }
    Proc *p = proc_alloc(0); // pid is filled in right after spawn
    if (!p) {
        console::println("run: no process slots left", ERR);
        return nullptr;
    }
    uint64_t root = paging::create_user_root();
    if (!root) {
        console::println("run: out of memory for address space", ERR);
        p->used = false;
        return nullptr;
    }
    p->root = root;
    p->owns_root = true;
    uint64_t brk = 0;
    elf::Loaded img = elf::load_as(root, data, size, brk);
    if (!img.ok) {
        if (size >= 4) {
            const uint8_t *b = (const uint8_t *)data;
            console::kprintf(ERR, BG, "run: '%s' not loadable (len=%u magic=%x%x%x%x)\n",
                             path, (unsigned)size, b[0], b[1], b[2], b[3]);
        } else {
            console::kprintf(ERR, BG, "run: '%s' not loadable (len=%u)\n",
                             path, (unsigned)size);
        }
        cleanup(*p);
        return nullptr;
    }
    p->brk = brk ? brk : 0x500000ULL;
    p->brk_start = p->brk;
    set_comm(*p, path);
    p->aux_phdr = img.phdr_vaddr;
    p->aux_phent = img.phent;
    p->aux_phnum = img.phnum;
    p->aux_entry = img.entry;
    if (!map_ustack(*p)) {
        console::println("run: out of memory for the user stack", ERR);
        cleanup(*p);
        return nullptr;
    }
    // argv[0] defaults to the path so programs always see a name.
    char *av[ARGV_MAX]{};
    char *avp[ARGV_MAX]{};
    int ac = 0;
    av[ac++] = (char *)path;
    for (int i = 0; i < argc && ac < (int)ARGV_MAX; i++)
        if (argv && argv[i]) av[ac++] = argv[i];
    for (int i = 0; i < ac; i++) avp[i] = av[i];
    uint64_t top = push_args(*p, ac, avp);
    if (!top) {
        console::println("run: arguments too large for the user stack", ERR);
        cleanup(*p);
        return nullptr;
    }
    auto *b = (Bootstrap *)heap::alloc(sizeof(Bootstrap));
    if (!b) {
        console::println("run: out of memory", ERR);
        cleanup(*p);
        return nullptr;
    }
    b->entry = img.entry;
    b->stack_top = top;
    p->thread = nullptr;
    p->done = false;
    p->code = -1;
    p->kill_me = false;
    p->pid = 0;
    p->ppid = 1;
    util::memset(p->sig, 0, sizeof(p->sig));
    p->sigmask = 0;
    p->sigpending = 0;
    *boot_out = b;
    return p;
}

long run_file(const char *path) { return run_args(path, 0, nullptr); }

long run_args(const char *path, int argc, char **argv) {
    return run_args_redir(path, argc, argv, nullptr, false, nullptr);
}

long run_args_redir(const char *path, int argc, char **argv, const char *out_path,
                    bool append, const char *in_path) {
    if (foreground_active()) {
        console::println("a foreground task is already running", ERR);
        return -1;
    }
    Bootstrap *b = nullptr;
    Proc *p = load_proc(path, argc, argv, &b);
    if (!p) return -1;
    uint64_t root = p->root;
    uint64_t entry = b->entry;
    syscall::reset_files();
    tty::init(); // fresh line discipline for this job (canonical, echo)
    // Redirect setup: open the sink/source before the job starts so its very
    // first write/read already lands in the right place.
    long out_id = -1;
    if (out_path) {
        const uint8_t *od = nullptr;
        size_t osz = 0;
        out_id = fs::writable_open(out_path, true, !append, &od, &osz);
        if (out_id < 0) {
            console::kprintf(ERR, BG, "run: cannot open output '%s'\n", out_path);
            heap::free(b);
            cleanup(*p);
            return -1;
        }
    }
    const void *in_data = nullptr;
    size_t in_size = 0;
    if (in_path && !fs::read(in_path, &in_data, &in_size)) {
        console::kprintf(ERR, BG, "run: cannot open input '%s'\n", in_path);
        heap::free(b);
        cleanup(*p);
        return -1;
    }
    // Interrupts off across spawn + redirect install: a timer tick here could
    // otherwise schedule the new thread before its stdio is wired up.
    cpu::cli();
    uint32_t child_pid = 0;
    if (!sched::spawn_user("user", bootstrap, b, root, 4, &child_pid)) {
        cpu::sti();
        console::println("run: no thread slots left", ERR);
        heap::free(b);
        cleanup(*p);
        return -1;
    }
    // The child pid is known right after spawn (bootstrap would set it later),
    // so its stdio can be wired before the first schedule. Publishing the pid
    // here also makes cur() resolve to this Proc for the child thread.
    p->pid = child_pid;
    s_fg_pid = child_pid;
    linux::reset_for(child_pid); // per-process mmap arena + FS base
    if (out_id >= 0) syscall::set_std_out(child_pid, out_id);
    if (in_path) syscall::set_std_in(child_pid, in_data, in_size);
    cpu::sti();
    syscall::set_tty_pgid(child_pid);
    console::kprintf(FG, BG, "[proc] '%s' entry=0x%lx brk=0x%lx, ring 3 (Ctrl+C kills)\n",
                     path, entry, p->brk);

    uint64_t last = pit::seconds();
    while (!p->done) {
        sched::yield();
        // Pump the desktop while blocked: mouse, repaint, clock. The focused
        // user window (e.g. fileman) consumes keys through the display server;
        // everything else is cooked by the console tty line discipline, which
        // raises SIGINT/SIGQUIT on ISIG control characters itself.
        char c;
        while (kbd::pop(c)) {
            if (gui::route_key(c)) continue;
            tty::feed(c);
        }
        gui::tick();
        uint64_t s = pit::seconds();
        if (s != last) {
            last = s;
            console::status("running %s (%llu s, Ctrl+C kills)...", path, s);
        }
        asm volatile("hlt");
    }
    long code = p->code;
    bool killed = p->kill_me;
    s_fg_pid = 0;
    if (killed)
        console::println("[proc] killed.", ERR);
    else
        console::kprintf(OK, BG, "[proc] exit code %ld.\n", code);
    cleanup(*p); // frees the private address space + the process slot
    sched::reap_pid(child_pid); // free the foreground thread slot (no wait4 for it)
    syscall::set_tty_pgid(1);
    if (out_id >= 0) fs::disk_persistence_flush(); // persist a redirected file
    return code;
}

// Launch a program as a detached job and return at once: the WM keeps pumping
// the desktop while the app runs on its own thread, and several jobs coexist.
// Unlike run_args_redir this must not touch the shared foreground state
// (s_fg_pid, the console tty, the Linux mmap arena) nor reset the other
// processes' descriptors, or a running app would be torn down by a new launch.
long spawn_job(const char *path, int argc, char **argv, uint32_t *pid_out) {
    Bootstrap *b = nullptr;
    Proc *p = load_proc(path, argc, argv, &b);
    if (!p) return -1;
    uint64_t root = p->root;
    p->job = true;
    cpu::cli();
    uint32_t child_pid = 0;
    if (!sched::spawn_user("user", bootstrap, b, root, 4, &child_pid)) {
        cpu::sti();
        console::println("run: no thread slots left", ERR);
        heap::free(b);
        cleanup(*p);
        return -1;
    }
    p->pid = child_pid;
    linux::reset_for(child_pid); // per-process mmap arena + FS base (pid may be reused)
    // A fresh, empty descriptor table for this job only. The pid may be reused,
    // so clear it explicitly rather than assuming s_fds[] was already zero.
    syscall::reset_files_for(child_pid);
    cpu::sti();
    if (pid_out) *pid_out = child_pid;
    console::kprintf(FG, BG, "[proc] '%s' job pid=%u, ring 3\n", path, child_pid);
    return 0;
}

// Reap finished jobs. Called from the ambient GUI loop (gui::tick): a job has
// no waiting thread, and its own thread is still running on the address space
// at exit, so the teardown happens here, on a thread sitting on the kernel root.
void reap_jobs() {
    for (auto &p : s_procs) {
        if (!p.used || !p.job || !p.done) continue;
        uint32_t pid = p.pid;
        long code = p.code;
        bool killed = p.kill_me;
        cleanup(p);
        syscall::drop_files(pid);
        sched::reap_pid(pid);
        if (killed) console::kprintf(FG, BG, "[proc] job pid=%u killed.\n", pid);
        else console::kprintf(FG, BG, "[proc] job pid=%u exit code %ld.\n", pid, code);
    }
}

// Terminate a detached job from outside (the WM window close button). The app
// itself asked nothing, so unlike raise_signal no handler can defer it.
bool kill_job(uint32_t pid) {
    Proc *p = proc_of(pid);
    if (!p || !p->job || p->done) return false;
    p->kill_me = true;
    p->code = 128 + 15; // SIGTERM
    p->done = true;
    // Stop the thread and any clone that shares its group; the ambient loop
    // frees the address space once the thread is off it.
    sched::signal_group(pid, 15);
    return true;
}

} // namespace choco::proc

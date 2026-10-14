// Linux syscall translator, part 1: MSR setup + address-space helpers.
// Dispatch lives in syscall.cpp (it reuses the file/pointer validators);
// this file owns the EFER/STAR/LSTAR/FMASK setup and the per-foreground
// mmap area + FS base (single task at a time, like everything in phase 5+).
#include <choco/cpu.hpp>
#include <choco/mem.hpp>
#include <choco/memfd.hpp>
#include <choco/sched.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

extern "C" void linux_syscall_entry();

namespace choco::linux {
namespace {

inline constexpr uint64_t MSR_EFER = 0xC0000080;
inline constexpr uint64_t MSR_STAR = 0xC0000081;
inline constexpr uint64_t MSR_LSTAR = 0xC0000082;
inline constexpr uint64_t MSR_FMASK = 0xC0000084;
inline constexpr uint64_t MSR_FS_BASE = 0xC0000100;

inline uint64_t rdmsr(uint64_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
inline void wrmsr(uint64_t msr, uint64_t v) {
    asm volatile("wrmsr" ::"a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(msr));
}

constexpr uint64_t MMAP_BASE = 0x70000000ULL; // linux mmap arena (bump)
constexpr uint64_t MMAP_END = 0x70000000ULL + 0x40000000ULL;
// Per-process ceiling on the anonymous mmap arena (Fase 18 resource limit:
// without it one program can drain the PMM and starve the kernel).
constexpr uint64_t MMAP_TOTAL_LIMIT = 128ULL * 1024 * 1024;
// Per-process Linux translator state (mmap bump arena + FS base), keyed by pid.
// More than one process can run at once now (background jobs), so this cannot
// be a single global.
struct LtState {
    uint64_t mmap_next = MMAP_BASE;
    uint64_t mmap_used = 0;
    uint64_t fsbase = 0;
    // VA ranges mapped MAP_SHARED from a memfd: munmap() must unmap them but
    // must NOT free the physical pages, which belong to the object (and may be
    // mapped in another process, e.g. the compositor).
    struct Shm { uint64_t base = 0, len = 0; };
    Shm shm[16];
    unsigned nshm = 0;
};
constexpr unsigned LT_N = 64;
LtState s_lt[LT_N];
LtState &lt_of(uint32_t pid) { return s_lt[pid & (LT_N - 1)]; }
LtState &cur_lt() { return lt_of(sched::current_pid()); }

} // namespace

void init() {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1ULL); // SCE: enable SYSCALL/SYSRET
    // STAR: kernel CS=0x08 (SS follows at +8); user half unused (iretq
    // return path), set to a sane 0x10 base anyway.
    wrmsr(MSR_STAR, (0x08ULL << 32) | (0x10ULL << 48));
    wrmsr(MSR_LSTAR, (uint64_t)&linux_syscall_entry);
    wrmsr(MSR_FMASK, 0x700ULL); // clear TF/IF/DF on entry (like int $0x80)
    // Enable SSE/FPU for user space (musl emits SSE2): the kernel itself
    // stays integer-only (-mno-sse), so no per-thread FPU save/restore is
    // needed while a single foreground task runs at a time.
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); // EM=0: FPU present
    cr0 |= (1ULL << 1);  // MP=1: x87 faults route correctly
    asm volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); // OSFXSR + OSXMMEXCPT
    asm volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    asm volatile("fninit");
}

void reset() {
    LtState &l = cur_lt();
    l.mmap_next = MMAP_BASE;
    l.mmap_used = 0;
    l.fsbase = 0;
    l.nshm = 0;
}

// Clear a specific process's translator state (used when a job is set up, before
// its thread runs, so cur_lt() would resolve to the wrong pid).
void reset_for(uint32_t pid) {
    LtState &l = lt_of(pid);
    l.mmap_next = MMAP_BASE;
    l.mmap_used = 0;
    l.fsbase = 0;
    l.nshm = 0;
}

// Load a process's FS base on a context switch (another process may have set it).
void switch_fs(uint32_t pid) { wrmsr(MSR_FS_BASE, lt_of(pid).fsbase); }

// A forked child inherits the parent's TLS base and mmap arena bookkeeping. The
// address space itself is copied by the caller; this keeps the Linux translator
// state consistent (a musl child accesses TLS at FS:0 and #PFs if fsbase is 0).
void clone_state(uint32_t from_pid, uint32_t to_pid) {
    lt_of(to_pid) = lt_of(from_pid);
}

// Reserve a VA range in the per-process mmap arena (bump allocator). Fixed
// addresses are honored when still free. No pages are mapped here.
static uint64_t reserve_va(uint64_t hint, uint64_t len) {
    uint64_t root = proc::proc_root();
    if (!root || len == 0 || len > 64 * 1024 * 1024) return (uint64_t)-12;
    len = (len + 0xFFFULL) & ~0xFFFULL;
    if (cur_lt().mmap_used + len > MMAP_TOTAL_LIMIT) return (uint64_t)-12; // ENOMEM
    uint64_t base = 0;
    if (hint) {
        hint &= ~0xFFFULL;
        if (hint < 0x0000800000000000ULL && hint + len > hint &&
            hint + len <= 0x0000800000000000ULL) {
            bool free = true;
            for (uint64_t v = hint; v < hint + len; v += 4096)
                if (paging::translate_as(root, v)) { free = false; break; }
            if (free) base = hint;
        }
        if (!base) return (uint64_t)-12;
    } else {
        base = (cur_lt().mmap_next + 0xFFFULL) & ~0xFFFULL;
        if (base + len > MMAP_END || base + len < base) return (uint64_t)-12;
        cur_lt().mmap_next = base + len;
    }
    cur_lt().mmap_used += len;
    return base;
}

// Anonymous private mapping. Fixed addresses honored when free.
static uint64_t map_arena(uint64_t hint, uint64_t len) {
    uint64_t base = reserve_va(hint, len);
    if ((int64_t)base < 0) return base;
    uint64_t root = proc::proc_root();
    const uint64_t sz = (len + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t v = base; v < base + sz; v += 4096) {
        uint64_t ph = pmm::alloc_zero();
        if (!ph) return (uint64_t)-12; // partial: reaped at exit
        if (!paging::map_as(root, v, ph,
                             paging::F_PRESENT | paging::F_RW | paging::F_USER)) {
            pmm::free(ph);
            return (uint64_t)-12;
        }
    }
    return base;
}

uint64_t mmap(uint64_t hint, uint64_t len) { return map_arena(hint, len); }

// MAP_SHARED mapping of a memfd object. The same physical pages are mapped, so
// a write by one process is visible to any other that mapped the object.
uint64_t mmap_shared(uint64_t hint, uint64_t len, uint64_t off, int memfd_id) {
    if (off % 4096) return (uint64_t)-22;
    const uint64_t sz = (len + 0xFFFULL) & ~0xFFFULL;
    uint64_t base = reserve_va(hint, sz);
    if ((int64_t)base < 0) return base;
    uint64_t root = proc::proc_root();
    for (uint64_t v = base; v < base + sz; v += 4096) {
        uint64_t ph = memfd::page_phys(memfd_id, (off + (v - base)) / 4096);
        if (!ph) return (uint64_t)-12;
        if (!paging::map_as(root, v, ph,
                            paging::F_PRESENT | paging::F_RW | paging::F_USER))
            return (uint64_t)-12;
    }
    LtState &l = cur_lt();
    if (l.nshm < 16) { l.shm[l.nshm++] = { base, sz }; }
    return base;
}

// File-backed private mapping: fresh pages pre-filled with src[off..off+len),
// zero past EOF. MAP_PRIVATE, so writes never reach the backing file.
uint64_t mmap_file(uint64_t hint, uint64_t len, const void *src, size_t src_len,
                   uint64_t off) {
    uint64_t base = map_arena(hint, len);
    if ((int64_t)base < 0) return base;
    uint64_t root = proc::proc_root();
    const uint8_t *s = (const uint8_t *)src;
    const uint64_t sz = (len + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t v = base; v < base + sz; v += 4096) {
        uint64_t ph = paging::translate_as(root, v);
        if (!ph) continue;
        uint8_t *dst = (uint8_t *)paging::phys_to_virt(ph);
        const uint64_t fofs = off + (v - base);
        for (uint64_t i = 0; i < 4096; i++)
            dst[i] = (s && fofs + i < src_len) ? s[fofs + i] : 0;
    }
    return base;
}

// PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4, PROT_NONE=0. Applies the real PTE
// permissions (no NX bit yet: EXEC maps as readable, forbidden NX is out of
// scope without EFER.NXE).
long mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    uint64_t root = proc::proc_root();
    if (!root) return -22;
    addr &= ~0xFFFULL;
    len = (len + 0xFFFULL) & ~0xFFFULL;
    if (addr + len < addr || addr + len > 0x0000800000000000ULL) return -22;
    uint64_t flags = 0;
    if (prot & 0x7) flags |= paging::F_PRESENT | paging::F_USER; // R/W/X => present
    if (prot & 0x2) flags |= paging::F_RW;
    for (uint64_t v = addr; v < addr + len; v += 4096) paging::protect_as(root, v, flags);
    return 0;
}

long munmap(uint64_t addr, uint64_t len) {
    uint64_t root = proc::proc_root();
    if (!root) return -22;
    addr &= ~0xFFFULL;
    len = (len + 0xFFFULL) & ~0xFFFULL;
    if (addr + len < addr || addr + len > 0x0000800000000000ULL) return -22;
    LtState &l = cur_lt();
    // A MAP_SHARED range's pages belong to the memfd object (possibly mapped in
    // another process), so unmap without freeing them.
    bool shared = false;
    for (unsigned i = 0; i < l.nshm; i++) {
        const LtState::Shm &s = l.shm[i];
        if (addr >= s.base && addr + len <= s.base + s.len) { shared = true; break; }
    }
    if (shared) {
        for (uint64_t v = addr; v < addr + len; v += 4096) paging::unmap_as(root, v);
    } else {
        for (uint64_t v = addr; v < addr + len; v += 4096) {
            uint64_t ph = paging::translate_as(root, v);
            if (ph) {
                paging::unmap_as(root, v);
                pmm::free(ph & ~0xFFFULL);
            }
        }
    }
    l.mmap_used = l.mmap_used > len ? l.mmap_used - len : 0;
    return 0;
}

// Absolute break (Linux brk semantic). Growth delegates to proc_sbrk.
long brk(uint64_t addr) {
    if (!proc::proc_root()) return -22;
    uint64_t cur = proc::proc_brk();
    if (!cur) return -12;
    if (addr == 0) return (long)cur;
    if (addr == cur) return (long)cur;
    if (addr > cur) {
        if (addr >= 0x0000800000000000ULL) return -12;
        uint64_t old = proc::proc_sbrk((int64_t)(addr - cur));
        if (old == (uint64_t)-1) return -12;
        return (long)proc::proc_brk();
    }
    // No shrink: keep the break monotonic (musl only trims opportunistically;
    // refusing is safe, just RSS). Unmapping here would fault later touches.
    return (long)cur;
}

long set_fs(uint64_t addr) {
    if (addr >= 0x0000800000000000ULL) return -22;
    cur_lt().fsbase = addr;
    wrmsr(MSR_FS_BASE, addr);
    return 0;
}

uint64_t get_fs() { return cur_lt().fsbase; }

} // namespace choco::linux

// C entry from linux.asm (same cpu::Frame layout as isr.asm).
extern "C" long linux_dispatch(choco::cpu::Frame *f);

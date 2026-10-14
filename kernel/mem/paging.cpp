// 4-level paging: builds our own PML4, mirrors HHDM + kernel, switches CR3.
#include <limine.h>
#include <choco/boot.hpp>
#include <choco/console.hpp>
#include <choco/mem.hpp>
#include <choco/util.hpp>

using choco::console::ACC;
using choco::console::BG;
using choco::console::FG;
using choco::console::ERR;

namespace choco::paging {
namespace {
uint64_t s_pml4_phys = 0;
uint64_t s_hhdm = 0;

inline void invlpg(uint64_t v) { asm volatile("invlpg (%0)" ::"r"(v) : "memory"); }

inline uint64_t *table_virt(uint64_t phys) { return (uint64_t *)(phys + s_hhdm); }

// Mask for the address field of a page-table entry. Never use ~0xFFFULL:
// entries may carry flag bits above bit 51 (notably NX bit 63, which Limine
// sets on data pages); keeping them poisons the next table pointer.
inline constexpr uint64_t E_ADDR_MASK = 0x000FFFFFFFFFF000ULL;

uint64_t *walk_create(uint64_t *lvl, uint64_t idx, bool user) {
    uint64_t e = lvl[idx];
    if (e & F_PRESENT) return table_virt(e & E_ADDR_MASK);
    uint64_t np = pmm::alloc_zero();
    if (!np) return nullptr;
    // The USER bit must be set on EVERY level of a user walk, so propagate
    // it. Kernel-only subtrees (PML4[511], HHDM slot) stay supervisor: low
    // PML4 slots are only ever used by user mappings.
    lvl[idx] = np | F_PRESENT | F_RW | (user ? F_USER : 0);
    return table_virt(np);
}

void free_table_level(uint64_t tab_phys, unsigned level) {
    // level: 3=PDPT 2=PD 1=PT. Frees leaf frames at PT, recurses above.
    // User space never uses huge pages; a huge entry here would be a bug,
    // so just drop it instead of freeing a wrong-sized frame.
    uint64_t *tab = table_virt(tab_phys);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = tab[i];
        if (!(e & F_PRESENT)) continue;
        if (e & F_HUGE) { tab[i] = 0; continue; }
        uint64_t child = e & E_ADDR_MASK;
        if (level > 1) free_table_level(child, level - 1);
        else pmm::free(child); // PT slot = user frame
        tab[i] = 0;
    }
    pmm::free(tab_phys);
}

uint64_t *walk_get(uint64_t *lvl, uint64_t idx) {
    uint64_t e = lvl[idx];
    if (!(e & F_PRESENT)) return nullptr;
    return table_virt(e & E_ADDR_MASK);
}
} // namespace

uint64_t current_pml4_phys() { return s_pml4_phys; }
uint64_t hhdm() { return s_hhdm; }
void *phys_to_virt(uint64_t phys) { return (void *)(phys + s_hhdm); }

bool map(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!s_pml4_phys) return false;
    virt &= ~0xFFFULL; phys &= ~0xFFFULL;
    // Exactly THREE table levels (PML4 -> PDPT -> PD -> PT); the PT slot
    // itself holds the final frame. A fourth walk would treat the PT as a
    // table, leaking one page table per mapped page (and mistranslating).
    uint64_t *pml4 = table_virt(s_pml4_phys);
    bool user = (flags & F_USER) != 0;
    uint64_t *pdpt = walk_create(pml4, (virt >> 39) & 0x1FF, user); if (!pdpt) return false;
    uint64_t *pd = walk_create(pdpt, (virt >> 30) & 0x1FF, user); if (!pd) return false;
    uint64_t *pt = walk_create(pd, (virt >> 21) & 0x1FF, user); if (!pt) return false;
    pt[(virt >> 12) & 0x1FF] = phys | (flags & 0xFFFULL) | F_PRESENT;
    invlpg(virt);
    return true;
}

bool unmap(uint64_t virt) {
    if (!s_pml4_phys) return false;
    virt &= ~0xFFFULL;
    uint64_t *pml4 = table_virt(s_pml4_phys);
    uint64_t *pdpt = walk_get(pml4, (virt >> 39) & 0x1FF); if (!pdpt) return false;
    uint64_t *pd = walk_get(pdpt, (virt >> 30) & 0x1FF); if (!pd) return false;
    uint64_t *pt = walk_get(pd, (virt >> 21) & 0x1FF); if (!pt) return false;
    pt[(virt >> 12) & 0x1FF] = 0;
    invlpg(virt);
    return true;
}

bool unmap_as(uint64_t root_phys, uint64_t virt) {
    if (!root_phys) return false;
    virt &= ~0xFFFULL;
    uint64_t *pml4 = table_virt(root_phys);
    uint64_t *pdpt = walk_get(pml4, (virt >> 39) & 0x1FF); if (!pdpt) return false;
    uint64_t *pd = walk_get(pdpt, (virt >> 30) & 0x1FF); if (!pd) return false;
    uint64_t *pt = walk_get(pd, (virt >> 21) & 0x1FF); if (!pt) return false;
    pt[(virt >> 12) & 0x1FF] = 0;
    if (root_phys == read_cr3()) invlpg(virt);
    return true;
}

bool protect_as(uint64_t root_phys, uint64_t virt, uint64_t flags) {
    if (!root_phys) return false;
    virt &= ~0xFFFULL;
    uint64_t *pml4 = table_virt(root_phys);
    uint64_t *pdpt = walk_get(pml4, (virt >> 39) & 0x1FF); if (!pdpt) return false;
    uint64_t *pd = walk_get(pdpt, (virt >> 30) & 0x1FF); if (!pd) return false;
    uint64_t *pt = walk_get(pd, (virt >> 21) & 0x1FF); if (!pt) return false;
    uint64_t &e = pt[(virt >> 12) & 0x1FF];
    if (!(e & F_PRESENT)) return false;              // only protect mapped pages
    e = (e & ~0xFFFULL) | (flags & 0xFFFULL);        // keep frame (+ NX bit)
    if (root_phys == read_cr3()) invlpg(virt);
    return true;
}

uint64_t translate_root(uint64_t root_phys, uint64_t virt) {
    // Walk an arbitrary PML4 (ours or Limine's). table_virt() works for both:
    // the HHDM offset is identical, and all page tables live in mapped RAM.
    // Handles 4K, 2MB and (defensively) 1GB pages: Limine maps the kernel
    // image with large pages.
    uint64_t e = table_virt(root_phys)[(virt >> 39) & 0x1FF];
    if (!(e & F_PRESENT)) return 0;
    uint64_t *pdpt = table_virt(e & E_ADDR_MASK);
    e = pdpt[(virt >> 30) & 0x1FF];
    if (!(e & F_PRESENT)) return 0;
    if (e & F_HUGE) return (e & E_ADDR_MASK) + (virt & 0x3FFFFFFFULL);
    uint64_t *pd = table_virt(e & E_ADDR_MASK);
    e = pd[(virt >> 21) & 0x1FF];
    if (!(e & F_PRESENT)) return 0;
    if (e & F_HUGE) return (e & E_ADDR_MASK) + (virt & 0x1FFFFFULL);
    uint64_t *pt = table_virt(e & E_ADDR_MASK);
    e = pt[(virt >> 12) & 0x1FF];
    if (!(e & F_PRESENT)) return 0;
    return (e & E_ADDR_MASK) + (virt & 0xFFFULL);
}

uint64_t translate(uint64_t virt) {
    // Must walk the CURRENT address space, not the boot PML4: user threads
    // run on private roots, and syscall pointer validation depends on this.
    // (On kernel threads current == boot PML4, so boot-time users are safe.)
    return translate_root(read_cr3(), virt);
}

uint64_t translate_as(uint64_t root_phys, uint64_t virt) {
    if (!root_phys) return 0;
    return translate_root(root_phys, virt);
}

bool map_as(uint64_t root_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!root_phys) return false;
    virt &= ~0xFFFULL; phys &= ~0xFFFULL;
    uint64_t *pml4 = table_virt(root_phys);
    bool user = (flags & F_USER) != 0;
    uint64_t *pdpt = walk_create(pml4, (virt >> 39) & 0x1FF, user); if (!pdpt) return false;
    uint64_t *pd = walk_create(pdpt, (virt >> 30) & 0x1FF, user); if (!pd) return false;
    uint64_t *pt = walk_create(pd, (virt >> 21) & 0x1FF, user); if (!pt) return false;
    pt[(virt >> 12) & 0x1FF] = phys | (flags & 0xFFFULL) | F_PRESENT;
    if (root_phys == s_pml4_phys || root_phys == read_cr3()) invlpg(virt);
    return true;
}

uint64_t create_user_root() {
    if (!s_pml4_phys) return 0;
    uint64_t np = pmm::alloc_zero();
    if (!np) return 0;
    uint64_t *src = table_virt(s_pml4_phys);
    uint64_t *dst = table_virt(np);
    // Low half (0..255) stays empty: pure user space.
    // High half (256..511) shares the kernel tables (HHDM, image, stacks).
    for (unsigned i = 256; i < 512; i++) dst[i] = src[i];
    return np;
}

namespace {
uint64_t clone_level(uint64_t src_phys, unsigned level) {
    uint64_t dst_phys = pmm::alloc_zero();
    if (!dst_phys) return 0;
    auto *src = table_virt(src_phys);
    auto *dst = table_virt(dst_phys);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & F_PRESENT)) continue;
        uint64_t flags = e & 0xFFFULL;
        uint64_t child = e & E_ADDR_MASK;
        if (level == 1) {
            uint64_t page = pmm::alloc_zero();
            if (!page) return 0;
            util::memcpy(table_virt(page), table_virt(child), 4096);
            dst[i] = page | flags;
        } else {
            uint64_t copy = clone_level(child, level - 1);
            if (!copy) return 0;
            dst[i] = copy | flags;
        }
    }
    return dst_phys;
}
}

uint64_t clone_user_root(uint64_t root_phys) {
    if (!root_phys) return 0;
    uint64_t np = pmm::alloc_zero();
    if (!np) return 0;
    auto *src = table_virt(root_phys);
    auto *dst = table_virt(np);
    for (unsigned i = 256; i < 512; i++) dst[i] = src[i];
    for (unsigned i = 0; i < 256; i++) {
        if (!(src[i] & F_PRESENT)) continue;
        uint64_t copy = clone_level(src[i] & E_ADDR_MASK, 3);
        if (!copy) {
            destroy_user_root(np);
            return 0;
        }
        dst[i] = copy | (src[i] & 0xFFFULL);
    }
    return np;
}

void destroy_user_root(uint64_t root_phys) {
    if (!root_phys || root_phys == s_pml4_phys) return;
    uint64_t *pml4 = table_virt(root_phys);
    // Only the low half belongs to the process; the high half is shared.
    for (unsigned i = 0; i < 256; i++) {
        uint64_t e = pml4[i];
        if (!(e & F_PRESENT)) continue;
        free_table_level(e & E_ADDR_MASK, 3);
        pml4[i] = 0;
    }
    pmm::free(root_phys);
}

void init() {
    auto &bi = boot::info();
    s_hhdm = bi.hhdm;

    s_pml4_phys = pmm::alloc_zero();

    // 1. whole memmap reachable via HHDM (covers RAM, Limine stack, bitmap, fb).
    // Only usable-class entries plus the framebuffer: newer Limine versions
    // report gigantic non-usable ranges that would take ages to map page by
    // page (same trap as the PMM bitmap sizing).
    auto keep = [](uint64_t t) {
        return t == LIMINE_MEMMAP_USABLE || t == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
               t == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES || t == LIMINE_MEMMAP_FRAMEBUFFER;
    };
    for (uint64_t i = 0; i < bi.memmap->entry_count; i++) {
        auto *e = bi.memmap->entries[i];
        if (!keep(e->type)) continue;
        uint64_t base = e->base & ~0xFFFULL;
        uint64_t end = ((e->base + e->length + 0xFFF) & ~0xFFFULL);
        console::kprintf(FG, BG, "[paging] entry %llu base=0x%lx pages=%llu allocs=%llu\n",
                         i, base, (end - base) / 4096, pmm::alloc_calls());
        for (uint64_t p = base; p < end; p += 4096)
            map(s_hhdm + p, p, F_PRESENT | F_RW);
        console::kprintf(FG, BG, "[paging] entry %llu done, allocs=%llu free=%llu\n",
                         i, pmm::alloc_calls(), pmm::free_bytes() / 4096);
    }

    // 2. kernel higher-half: mirror Limine's mapping page by page. Limine
    // may place segments anywhere (not a linear slide: .bss was observed far
    // from the file image), so copy each PTE instead of assuming phys_base.
    if (bi.kern_virt && bi.kern_len) {
        uint64_t limine_cr3 = read_cr3();
        uint64_t kv = bi.kern_virt & ~0xFFFULL;
        uint64_t kend = (bi.kern_virt + bi.kern_len + 0xFFFULL) & ~0xFFFULL;
        uint64_t copied = 0;
        for (uint64_t v = kv; v < kend; v += 4096) {
            uint64_t p = translate_root(limine_cr3, v);
            if (!p) continue; // alignment gap between segments
            if (!map(v, p & E_ADDR_MASK, F_PRESENT | F_RW)) break; // OOM: stop
            copied++;
        }
        console::kprintf(FG, BG, "[paging] kernel mirrored: %llu pages\n", copied);
    }

    // 3. keep low 2MB in the HHDM (legacy firmware users, avoids stray faults)
    for (uint64_t p = 0; p < 0x200000; p += 4096)
        map(s_hhdm + p, p, F_PRESENT | F_RW);

    // 2b. validate the critical mappings BEFORE committing to them: a
    // mismatch here means a blind triple fault after mov cr3.
    {
        struct Check { const char *name; uint64_t vma; };
        uint64_t limine_cr3 = read_cr3();
        uint64_t cur_rsp;
        asm volatile("mov %%rsp, %0" : "=r"(cur_rsp));
        // NOTE: no fb check here on purpose. Limine maps the framebuffer at
        // its own address which may differ from hhdm+phys; our 1:1 mapping
        // is verified by the working display instead.
        const Check checks[] = {
            {"rip-page", (uint64_t)&init & ~0xFFFULL},
            {"stack", cur_rsp & ~0xFFFULL},
        };
        for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            uint64_t want = translate_root(limine_cr3, checks[i].vma) & E_ADDR_MASK;
            uint64_t got = translate_root(s_pml4_phys, checks[i].vma) & E_ADDR_MASK;
            console::kprintf((want && want == got) ? FG : ERR, BG,
                             "[paging] check %s: want=0x%lx got=0x%lx %s\n",
                             checks[i].name, want, got,
                             (want && want == got) ? "OK" : "MISMATCH");
        }
    }

    uint64_t old = read_cr3();
    (void)old;

    console::kprintf(FG, BG, "[paging] tables ready, switching CR3...\n");

    // Commit. No stack switch needed: our HHDM covers all usable RAM at the
    // same offset as Limine's, so the current stack stays valid. (Switching
    // RSP here would discard the caller's return address and triple-fault
    // on ret.)
    write_cr3(s_pml4_phys);

    console::kprintf(FG, BG, "[paging] new PML4 phys=0x%lx hhdm=0x%lx (old CR3=0x%lx)\n",
                     s_pml4_phys, s_hhdm, old);
}

} // namespace choco::paging

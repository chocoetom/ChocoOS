// Bitmap PMM over the Limine memmap.
#include <limine.h>
#include <choco/boot.hpp>
#include <choco/console.hpp>
#include <choco/mem.hpp>
#include <choco/util.hpp>

using choco::console::ACC;
using choco::console::BG;
using choco::console::FG;

namespace choco::pmm {
namespace {
uint8_t *s_bitmap = nullptr;
uint64_t s_total_pages = 0, s_free_pages = 0;
uint64_t s_usable_pages = 0; // real RAM: sum of usable ranges, excludes the
                             // address span between them (e.g. QEMU's PCI hole
                             // at 4G makes top-of-address > installed RAM).
uint64_t s_bitmap_pages = 0, s_bitmap_phys = 0;
uint64_t s_hhdm = 0;
uint64_t s_alloc_calls = 0; // boot diagnostics: total alloc() calls

inline void bit_set(uint64_t i) { s_bitmap[i / 8] |= (uint8_t)(1 << (i % 8)); }
inline void bit_clear(uint64_t i) { s_bitmap[i / 8] &= (uint8_t)~(1 << (i % 8)); }
inline bool bit_get(uint64_t i) { return (s_bitmap[i / 8] >> (i % 8)) & 1; }

inline bool usable(uint64_t t) {
    return t == LIMINE_MEMMAP_USABLE || t == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
           t == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
}
inline void *phys2virt(uint64_t p) { return (void *)(p + s_hhdm); }
} // namespace

const char *memmap_type_name(uint64_t t) {
    switch (t) {
    case LIMINE_MEMMAP_USABLE: return "usable";
    case LIMINE_MEMMAP_RESERVED: return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return "acpi-rec";
    case LIMINE_MEMMAP_ACPI_NVS: return "acpi-nvs";
    case LIMINE_MEMMAP_BAD_MEMORY: return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "boot-rec";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kern/mod";
    case LIMINE_MEMMAP_FRAMEBUFFER: return "fb";
    default: return "?";
    }
}

void dump_map() {
    auto &bi = boot::info();
    console::println("-- memmap limine --", ACC);
    for (uint64_t i = 0; i < bi.memmap->entry_count; i++) {
        auto *e = bi.memmap->entries[i];
        console::kprintf(FG, BG, "  [%llu] base=0x%lx len=0x%lx %s\n",
                         i, e->base, e->length, memmap_type_name(e->type));
    }
}

void init() {
    auto &bi = boot::info();
    s_hhdm = bi.hhdm;

    // 1. top of RAM from USABLE entries only. Newer Limine versions report
    // huge non-usable ranges (e.g. RESERVED_MAPPED); sizing the bitmap by
    // those would waste megabytes and make first-fit alloc() crawl for ages.
    uint64_t top = 0;
    for (uint64_t i = 0; i < bi.memmap->entry_count; i++) {
        auto *e = bi.memmap->entries[i];
        if (!usable(e->type)) continue;
        uint64_t end = e->base + e->length;
        if (end > top) top = end;
    }
    s_total_pages = (top + PAGE - 1) / PAGE;
    uint64_t bitmap_bytes = (s_total_pages + 7) / 8;
    s_bitmap_pages = (bitmap_bytes + PAGE - 1) / PAGE;

    // 2. find a contiguous usable region for the bitmap
    uint64_t place = 0;
    for (uint64_t i = 0; i < bi.memmap->entry_count && !place; i++) {
        auto *e = bi.memmap->entries[i];
        if (!usable(e->type)) continue;
        uint64_t base = util::align_up(e->base, PAGE);
        uint64_t end = e->base + e->length;
        if (end > base && end - base >= s_bitmap_pages * PAGE) {
            // avoid low 1MB
            if (base < 0x100000) {
                base = 0x100000;
                base = util::align_up(base, PAGE);
                if (end <= base || end - base < s_bitmap_pages * PAGE) continue;
            }
            place = base;
        }
    }
    // fallback: use the usable base itself, even in low mem
    if (!place) {
        for (uint64_t i = 0; i < bi.memmap->entry_count && !place; i++) {
            auto *e = bi.memmap->entries[i];
            if (!usable(e->type)) continue;
            if (e->length >= s_bitmap_pages * PAGE) place = util::align_up(e->base, PAGE);
        }
    }

    s_bitmap_phys = place;
    s_bitmap = (uint8_t *)phys2virt(place);
    // mark everything used, then free the usable ranges
    util::memset(s_bitmap, 0xFF, s_bitmap_pages * PAGE);
    s_free_pages = 0;

    for (uint64_t i = 0; i < bi.memmap->entry_count; i++) {
        auto *e = bi.memmap->entries[i];
        if (!usable(e->type)) continue;
        uint64_t s = util::align_up(e->base, PAGE) / PAGE;
        uint64_t en = (e->base + e->length) / PAGE;
        for (uint64_t p = s; p < en && p < s_total_pages; p++) {
            if (bit_get(p)) { bit_clear(p); s_free_pages++; }
        }
    }
    // Snapshot real RAM before the reservations below carve into the free set:
    // kernel image, bitmap and the low 1MB are part of installed memory.
    s_usable_pages = s_free_pages;

    // 3. reserve: low 1MB, bitmap, kernel image
    auto reserve = [&](uint64_t base, uint64_t len) {
        uint64_t s = base / PAGE, e = (base + len + PAGE - 1) / PAGE;
        for (uint64_t p = s; p < e && p < s_total_pages; p++)
            if (!bit_get(p)) { bit_set(p); s_free_pages--; }
    };
    reserve(0, 0x100000);
    reserve(s_bitmap_phys, s_bitmap_pages * PAGE);
    if (bi.kern_phys && bi.kern_len) reserve(bi.kern_phys & ~0xFFFULL, bi.kern_len);
    else if (bi.kern_phys) reserve(bi.kern_phys & ~0xFFFULL, 4 * 1024 * 1024);
    // Boot modules (initrd): reserve under both address interpretations
    // (virtual vs physical+HHDM); the out-of-range one is a harmless no-op.
    // This must happen here: the module type counts as usable RAM above.
    for (uint64_t i = 0; i < bi.module_count && bi.modules; i++) {
        auto *m = bi.modules[i];
        if (!m || !m->size) continue;
        uint64_t a = (uint64_t)m->address;
        uint64_t sz = (m->size + 0xFFFULL) & ~0xFFFULL;
        reserve(a & ~0xFFFULL, sz);
        reserve((s_hhdm + a) & ~0xFFFULL, sz);
    }
}

uint64_t alloc() {
    // NOTE: printing here would recurse via heap-backed formatting; the
    // counter below is read by paging diagnostics instead. Keep quiet.
    s_alloc_calls++;
    for (uint64_t i = 0; i < s_total_pages; i++) {
        if (!bit_get(i)) {
            bit_set(i);
            s_free_pages--;
            return i * PAGE;
        }
    }
    return 0;
}

uint64_t alloc_calls() { return s_alloc_calls; }

uint64_t alloc_zero() {
    uint64_t p = alloc();
    if (p) util::memset(phys2virt(p), 0, PAGE);
    return p;
}

void free(uint64_t phys) {
    if (phys % PAGE) return;
    uint64_t i = phys / PAGE;
    if (i >= s_total_pages || !bit_get(i)) return; // ignore double-free
    bit_clear(i);
    s_free_pages++;
}

uint64_t total_bytes() { return s_usable_pages * PAGE; }
uint64_t free_bytes() { return s_free_pages * PAGE; }
uint64_t used_bytes() { return total_bytes() - free_bytes(); }
uint64_t total_pages() { return s_usable_pages; }

} // namespace choco::pmm

// Minimal ELF64 loader for user programs (ET_EXEC, x86-64).
#include <choco/mem.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

namespace choco::elf {
namespace {

struct Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN = 3;
constexpr uint16_t EM_X86_64 = 62;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_INTERP = 3;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_TLS = 7;

bool map_zeroed(uint64_t vaddr, uint64_t memsz) {
    // Map every page of [vaddr, vaddr+memsz), but never remap an already
    // present page: segments routinely share pages (e.g. hello's .text and
    // .rodata live in one 4K page), and remapping would leak the first
    // frame and wipe its bytes. Fresh frames come zeroed from alloc_zero,
    // so BSS needs no extra work and previously written bytes are preserved.
    uint64_t base = vaddr & ~0xFFFULL;
    uint64_t end = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t v = base; v < end; v += 4096) {
        if (paging::translate(v)) continue;
        uint64_t ph = pmm::alloc_zero(); // already zeroed
        if (!ph) return false;
        if (!paging::map(v, ph, paging::F_PRESENT | paging::F_RW | paging::F_USER)) {
            pmm::free(ph);
            return false;
        }
    }
    return true;
}

// Copy through the HHDM so the target space need not be loaded in CR3.
void copy_as(uint64_t root, uint64_t vaddr, const uint8_t *src, size_t n) {
    uint64_t hhdm = paging::hhdm();
    size_t off = 0;
    while (off < n) {
        uint64_t v = vaddr + off;
        uint64_t page_off = v & 0xFFFULL;
        uint64_t chunk = 4096 - page_off;
        if (chunk > n - off) chunk = n - off;
        uint64_t ph = paging::translate_as(root, v & ~0xFFFULL);
        uint8_t *dst = (uint8_t *)(ph + hhdm + page_off);
        util::memcpy(dst, src + off, (size_t)chunk);
        off += (size_t)chunk;
    }
}

bool map_zeroed_as(uint64_t root, uint64_t vaddr, uint64_t memsz) {
    uint64_t base = vaddr & ~0xFFFULL;
    uint64_t end = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t v = base; v < end; v += 4096) {
        if (paging::translate_as(root, v)) continue;
        uint64_t ph = pmm::alloc_zero();
        if (!ph) return false;
        if (!paging::map_as(root, v, ph,
                             paging::F_PRESENT | paging::F_RW | paging::F_USER)) {
            pmm::free(ph);
            return false;
        }
    }
    return true;
}

} // namespace

Loaded load(const void *file, size_t len) {
    Loaded out{};
    if (len < sizeof(Ehdr)) return out;
    const uint8_t *img = (const uint8_t *)file;
    // magic + class(64) + data(little) + version
    if (img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return out;
    if (img[4] != 2 || img[5] != 1 || img[6] != 1) return out;

    Ehdr h{};
    util::memcpy(&h, img, sizeof(h));
    if ((h.type != ET_EXEC && h.type != ET_DYN) || h.machine != EM_X86_64) return out;
    if (h.phnum == 0 || h.phnum > 16 || h.phentsize < sizeof(Phdr)) return out;
    if (h.phoff + (uint64_t)h.phnum * h.phentsize > len) return out;

    bool any = false;
    for (uint16_t i = 0; i < h.phnum; i++) {
        Phdr p{};
        util::memcpy(&p, img + h.phoff + (uint64_t)i * h.phentsize, sizeof(p));
        if (p.type != PT_LOAD || p.memsz == 0) continue;
        if (p.vaddr >= 0x0000800000000000ULL) return Loaded{}; // keep user in low half
        if (p.offset + p.filesz > len || p.filesz > p.memsz) return Loaded{};
        if (!map_zeroed(p.vaddr, p.memsz)) return Loaded{};
        util::memcpy((void *)p.vaddr, img + p.offset, (size_t)p.filesz);
        any = true;
    }
    if (!any || h.entry >= 0x0000800000000000ULL) return out;
    if (paging::translate(h.entry & ~0xFFFULL) == 0) return out;
    out.ok = true;
    out.entry = h.entry;
    return out;
}

Loaded load_as(uint64_t root, const void *file, size_t len, uint64_t &brk_out) {
    brk_out = 0;
    Loaded out{};
    if (!root || len < sizeof(Ehdr)) return out;
    const uint8_t *img = (const uint8_t *)file;
    if (img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return out;
    if (img[4] != 2 || img[5] != 1 || img[6] != 1) return out;

    Ehdr h{};
    util::memcpy(&h, img, sizeof(h));
    if ((h.type != ET_EXEC && h.type != ET_DYN) || h.machine != EM_X86_64) return out;
    if (h.phnum == 0 || h.phnum > 16 || h.phentsize < sizeof(Phdr)) return out;
    if (h.phoff + (uint64_t)h.phnum * h.phentsize > len) return out;

    bool any = false;
    uint64_t top = 0;
    for (uint16_t i = 0; i < h.phnum; i++) {
        Phdr p{};
        util::memcpy(&p, img + h.phoff + (uint64_t)i * h.phentsize, sizeof(p));
        if (p.type != PT_LOAD || p.memsz == 0) continue;
        if (p.vaddr >= 0x0000800000000000ULL) return Loaded{};
        if (p.offset + p.filesz > len || p.filesz > p.memsz) return Loaded{};
        if (!map_zeroed_as(root, p.vaddr, p.memsz)) return Loaded{};
        copy_as(root, p.vaddr, img + p.offset, (size_t)p.filesz);
        any = true;
        uint64_t end = p.vaddr + p.memsz;
        if (end > top) top = end;
    }
    if (!any || h.entry >= 0x0000800000000000ULL) return out;
    if (paging::translate_as(root, h.entry & ~0xFFFULL) == 0) return out;
    out.ok = true;
    out.entry = h.entry;
    brk_out = (top + 0xFFFULL) & ~0xFFFULL;
    // AT_PHDR: find the LOAD covering the phdr table (usually the first).
    out.phent = h.phentsize;
    out.phnum = h.phnum;
    for (uint16_t i = 0; i < h.phnum; i++) {
        Phdr p{};
        util::memcpy(&p, img + h.phoff + (uint64_t)i * h.phentsize, sizeof(p));
        if (p.type != PT_LOAD || p.filesz == 0) continue;
        if (p.offset <= h.phoff &&
            h.phoff + (uint64_t)h.phnum * h.phentsize <= p.offset + p.filesz) {
            out.phdr_vaddr = p.vaddr + (h.phoff - p.offset);
            break;
        }
    }
    return out;
}

} // namespace choco::elf

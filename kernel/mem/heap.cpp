// Kernel heap: free-list over a virtual area mapped on demand.
#include <choco/console.hpp>
#include <choco/mem.hpp>
#include <choco/util.hpp>

using choco::console::ACC;
using choco::console::BG;
using choco::console::FG;
using choco::console::OK;
using choco::console::ERR;

namespace choco::heap {
namespace {
struct Block {
    uint64_t magic; // 0xC0C0A
    size_t size;    // payload (sem header), ja alinhado
    bool free;
    Block *next;
    Block *prev;
};
inline constexpr uint64_t MAGIC = 0xC0C0A77ULL;
inline constexpr uint64_t BASE = 0xFFFFFFFFA0000000ULL;
inline constexpr size_t MAX = 64 * 1024 * 1024;
inline constexpr size_t INITIAL = 4 * 1024 * 1024;

uint8_t *s_base = (uint8_t *)BASE;
size_t s_mapped = 0;
Block *s_head = nullptr;
size_t s_used = 0;

bool grow(size_t need) {
    size_t want = util::align_up(need + sizeof(Block), 4096);
    if (s_mapped + want > MAX) return false;
    for (size_t o = 0; o < want; o += 4096) {
        uint64_t phys = pmm::alloc_zero();
        if (!phys) return false;
        if (!paging::map((uint64_t)(s_base + s_mapped + o), phys,
                         paging::F_PRESENT | paging::F_RW)) {
            pmm::free(phys);
            return false;
        }
    }
    // new free block at the end (or extend the last one when free)
    Block *nb = (Block *)(s_base + s_mapped);
    nb->magic = MAGIC; nb->size = want - sizeof(Block); nb->free = true;
    nb->next = nullptr; nb->prev = nullptr;
    if (!s_head) { s_head = nb; }
    else {
        Block *t = s_head;
        while (t->next) t = t->next;
        // coalesce when the last block is free and adjacent
        uint8_t *tend = (uint8_t *)t + sizeof(Block) + t->size;
        if (t->free && tend == (uint8_t *)nb) { t->size += sizeof(Block) + nb->size; }
        else { t->next = nb; nb->prev = t; }
    }
    s_mapped += want;
    return true;
}

void split(Block *b, size_t need) {
    if (b->size >= need + sizeof(Block) + 16)
        {
            Block *nb = (Block *)((uint8_t *)b + sizeof(Block) + need);
            nb->magic = MAGIC; nb->free = true;
            nb->size = b->size - need - sizeof(Block);
            nb->next = b->next; nb->prev = b;
            if (b->next) b->next->prev = nb;
            b->next = nb; b->size = need;
        }
}
} // namespace

void init() {
    s_mapped = 0; s_head = nullptr; s_used = 0;
    grow(INITIAL);
    console::kprintf(FG, BG, "[heap] base=0x%lx mapped=%u KiB\n", BASE, (unsigned)(s_mapped / 1024));
}

void *alloc(size_t size) {
    if (size == 0) size = 1;
    size = (size_t)util::align_up(size, 16);
    for (Block *b = s_head; b; b = b->next) {
        if (b->free && b->magic == MAGIC && b->size >= size) {
            split(b, size);
            b->free = false;
            s_used += b->size;
            util::memset((uint8_t *)b + sizeof(Block), 0, b->size);
            return (uint8_t *)b + sizeof(Block);
        }
    }
    size_t before = s_mapped;
    if (!grow(size)) return nullptr;
    (void)before;
    return alloc(size); // retry once (single-level recursion)
}

void free(void *p) {
    if (!p) return;
    Block *b = (Block *)((uint8_t *)p - sizeof(Block));
    if (b->magic != MAGIC || b->free) return; // double-free/corrompido: ignora
    util::memset(p, 0xDD, b->size);
    b->free = true;
    s_used -= b->size;
    // coalesce vizinhos
    if (b->next && b->next->free && b->next->magic == MAGIC &&
        (uint8_t *)b + sizeof(Block) + b->size == (uint8_t *)b->next) {
        b->size += sizeof(Block) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free && b->prev->magic == MAGIC &&
        (uint8_t *)b->prev + sizeof(Block) + b->prev->size == (uint8_t *)b) {
        b->prev->size += sizeof(Block) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

void *realloc(void *p, size_t ns) {
    if (!p) return alloc(ns);
    if (ns == 0) { free(p); return nullptr; }
    Block *b = (Block *)((uint8_t *)p - sizeof(Block));
    size_t old = (b->magic == MAGIC) ? b->size : 0;
    void *np = alloc(ns);
    if (!np) return nullptr;
    util::memcpy(np, p, old < ns ? old : ns);
    free(p);
    return np;
}

size_t mapped_bytes() { return s_mapped; }
size_t used_bytes() { return s_used; }

bool self_test() {
    void *a = alloc(64);
    void *b = alloc(4096);
    void *c = alloc(123);
    if (!a || !b || !c) return false;
    // padrao + verifica
    for (int i = 0; i < 64; i++) ((uint8_t *)a)[i] = (uint8_t)(i * 3 + 1);
    for (int i = 0; i < 64; i++) if (((uint8_t *)a)[i] != (uint8_t)(i * 3 + 1)) return false;
    // translate cobre o heap?
    if (paging::translate((uint64_t)a) == 0) return false;
    if (paging::translate((uint64_t)b) == 0) return false;
    // new/delete
    int *n = new int(0x12345678);
    if (!n || *n != 0x12345678) return false;
    delete n;
    free(a); free(b); free(c);
    // coalesce check: a big block must fit again
    void *big = alloc(INITIAL / 2);
    if (!big) return false;
    free(big);
    return true;
}

} // namespace choco::heap

extern "C" {
void *kmalloc(size_t s) { return choco::heap::alloc(s); }
void kfree(void *p) { choco::heap::free(p); }
}

// new/delete sobre o heap (sem exceptions)
void *operator new(size_t s) { return choco::heap::alloc(s); }
void *operator new[](size_t s) { return choco::heap::alloc(s); }
void operator delete(void *p) noexcept { choco::heap::free(p); }
void operator delete[](void *p) noexcept { choco::heap::free(p); }
void operator delete(void *p, size_t) noexcept { choco::heap::free(p); }
void operator delete[](void *p, size_t) noexcept { choco::heap::free(p); }

// Anonymous memory objects (memfd). See choco/memfd.hpp.
#include <choco/memfd.hpp>

#include <choco/mem.hpp>
#include <choco/serial.hpp>
#include <choco/util.hpp>

namespace choco::memfd {
namespace {

constexpr unsigned MEMFD_N = 16;
// Cap per object: enough for a full-screen ARGB buffer with slack.
constexpr uint64_t MAX_PAGES = 4096; // 16 MiB

struct Ent {
    bool used = false;
    int refs = 0;
    uint64_t npages = 0;
    uint64_t *pages = nullptr; // heap array of physical page addresses
};

Ent s_e[MEMFD_N];

bool valid(int id) { return id >= 0 && id < (int)MEMFD_N && s_e[id].used; }

} // namespace

int create() {
    for (int i = 0; i < (int)MEMFD_N; i++) {
        if (s_e[i].used) continue;
        s_e[i] = Ent{};
        s_e[i].used = true;
        s_e[i].refs = 1;
        return i;
    }
    return -24; // EMFILE
}

void ref(int id) {
    if (valid(id)) s_e[id].refs++;
}

void unref(int id) {
    if (!valid(id)) return;
    Ent &e = s_e[id];
    if (--e.refs > 0) return;
    for (uint64_t i = 0; i < e.npages; i++)
        if (e.pages[i]) pmm::free(e.pages[i]);
    if (e.pages) heap::free(e.pages);
    e = Ent{};
}

uint64_t page_phys(int id, uint64_t pageno) {
    if (!valid(id)) return 0;
    Ent &e = s_e[id];
    if (pageno >= e.npages) return 0;
    if (!e.pages[pageno]) e.pages[pageno] = pmm::alloc_zero();
    return e.pages[pageno];
}

uint64_t page_count(int id) { return valid(id) ? s_e[id].npages : 0; }

bool truncate(int id, uint64_t len) {
    if (!valid(id)) return false;
    Ent &e = s_e[id];
    uint64_t np = (len + 4095) / 4096;
    if (np > MAX_PAGES) return false;
    if (np == e.npages) return true;

    uint64_t *arr = nullptr;
    if (np) {
        arr = (uint64_t *)heap::alloc(sizeof(uint64_t) * (size_t)np);
        if (!arr) return false;
        for (uint64_t i = 0; i < np; i++) arr[i] = 0;
        const uint64_t keep = np < e.npages ? np : e.npages;
        for (uint64_t i = 0; i < keep; i++) arr[i] = e.pages[i];
        // Shrinking: free the tail pages we are dropping.
        for (uint64_t i = np; i < e.npages; i++)
            if (e.pages[i]) pmm::free(e.pages[i]);
    }
    if (e.pages) heap::free(e.pages);
    e.pages = arr;
    e.npages = np;
    return true;
}

uint64_t size(int id) { return valid(id) ? s_e[id].npages * 4096 : 0; }

size_t write_at(int id, uint64_t off, const void *src, size_t len) {
    if (!valid(id) || !src || !len) return 0;
    Ent &e = s_e[id];
    const uint8_t *s = (const uint8_t *)src;
    size_t done = 0;
    while (done < len) {
        uint64_t byte = off + done;
        uint64_t pg = byte / 4096;
        if (pg >= e.npages) break;
        if (!e.pages[pg]) e.pages[pg] = pmm::alloc_zero();
        if (!e.pages[pg]) break;
        uint8_t *p = (uint8_t *)paging::phys_to_virt(e.pages[pg]);
        uint64_t within = byte % 4096;
        size_t chunk = 4096 - within;
        if (chunk > len - done) chunk = len - done;
        util::memcpy(p + within, s + done, chunk);
        done += chunk;
    }
    return done;
}

size_t read_at(int id, uint64_t off, void *dst, size_t len) {
    if (!valid(id) || !dst || !len) return 0;
    Ent &e = s_e[id];
    uint8_t *d = (uint8_t *)dst;
    size_t done = 0;
    while (done < len) {
        uint64_t byte = off + done;
        uint64_t pg = byte / 4096;
        if (pg >= e.npages) break;
        uint8_t *p = e.pages[pg] ? (uint8_t *)paging::phys_to_virt(e.pages[pg]) : nullptr;
        uint64_t within = byte % 4096;
        size_t chunk = 4096 - within;
        if (chunk > len - done) chunk = len - done;
        for (size_t i = 0; i < chunk; i++) d[done + i] = p ? p[within + i] : 0;
        done += chunk;
    }
    return done;
}

} // namespace choco::memfd

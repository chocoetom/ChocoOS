// initrd: read-only USTAR tar from the first boot module.
#include <limine.h>
#include <choco/boot.hpp>
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/fat32.hpp>
#include <choco/mem.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;

namespace choco::fs {
namespace {

struct Entry {
    char name[100]{};
    const uint8_t *data = nullptr;
    size_t size = 0;
};

constexpr size_t MAX_ENTRIES = 64; // must exceed the initrd file count
Entry s_entries[MAX_ENTRIES]{};
size_t s_count = 0;
constexpr size_t MAX_WRITABLE = 16, WRITABLE_CAP = 262144;
struct Writable {
    bool used = false;
    char name[100]{};
    uint8_t *data = nullptr;
    size_t size = 0;
};
Writable s_writable[MAX_WRITABLE]{};
constexpr size_t MAX_DIRS = 16;
struct Dir { bool used = false; char name[100]{}; };
Dir s_dirs[MAX_DIRS]{};
bool is_dir_internal(const char *path) {
    if (!path) return false;
    while (*path == '/') path++;
    for (auto &d : s_dirs) if (d.used && !util::strcmp(path, d.name)) return true;
    // Directories the initrd only implies: "bin/fileman" makes "bin" a folder.
    // Without this, parents_exist() rejects every create inside a directory, so
    // programs could only ever write files at the root.
    char want[104]{};
    size_t n = 0;
    for (; path[n] && n + 2 < sizeof(want); n++) want[n] = path[n];
    want[n] = '/';
    for (size_t i = 0; i < s_count; i++) {
        const char *nm = s_entries[i].name;
        size_t k = 0;
        while (want[k] && nm[k] == want[k]) k++;
        if (!want[k] && nm[k]) return true;  // a real child, not the name itself
    }
    return false;
}

bool parents_exist(const char *path) {
    char prefix[100]{};
    for (size_t i = 0; path[i]; i++) {
        if (path[i] != '/') continue;
        if (i == 0 || i >= sizeof(prefix)) return false;
        util::memcpy(prefix, path, i); prefix[i] = 0;
        if (!is_dir_internal(prefix)) return false;
    }
    return true;
}

uint64_t octal(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) + (uint64_t)(s[i] - '0');
    }
    return v;
}

bool is_ustar(const uint8_t *p) {
    return p[257] == 'u' && p[258] == 's' && p[259] == 't' &&
           p[260] == 'a' && p[261] == 'r';
}

// Limine hands us a module address that may be virtual already or physical
// (HHDM view). Probe safely: never touch a page translate() rejects.
const uint8_t *find_tar(const limine_file &mod, uint64_t hhdm) {
    uint64_t a = (uint64_t)mod.address;
    if (mod.size >= 512 && paging::translate(a & ~0xFFFULL)) {
        auto *p = (const uint8_t *)a;
        if (p[0] != 0 && is_ustar(p)) return p;
    }
    uint64_t v = hhdm + a;
    if (mod.size >= 512 && paging::translate(v & ~0xFFFULL)) {
        auto *p = (const uint8_t *)v;
        if (p[0] != 0 && is_ustar(p)) return p;
    }
    return nullptr;
}

// Any boot module that is not the initrd tar is a raw device image (the
// FAT32 ramdisk). Probe the address the same defensive way as find_tar.
const uint8_t *module_data(const limine_file &mod, uint64_t hhdm) {
    uint64_t a = (uint64_t)mod.address;
    if (mod.size >= 512 && paging::translate(a & ~0xFFFULL)) return (const uint8_t *)a;
    uint64_t v = hhdm + a;
    if (mod.size >= 512 && paging::translate(v & ~0xFFFULL)) return (const uint8_t *)v;
    return nullptr;
}

} // namespace

void init() {
    s_count = 0;
    for (auto &w : s_writable) {
        if (w.data) heap::free(w.data);
        w = Writable{};
    }
    for (auto &d : s_dirs) d = Dir{};
    auto &bi = boot::info();
    if (bi.module_count == 0 || !bi.modules || !bi.modules[0]) {
        console::println("[fs] no boot modules: initrd empty (ls/run unavailable)", FG);
        return;
    }
    auto *mod = bi.modules[0];
    const uint8_t *tar = find_tar(*mod, bi.hhdm);
    if (!tar) {
        console::println("[fs] first boot module is not a tar: initrd empty", FG);
    } else {
        // Copy to the heap: module pages stay reserved, but a private copy is
        // immune to any future mapping games.
        size_t tarsize = (size_t)mod->size;
        uint8_t *copy = (uint8_t *)heap::alloc(tarsize);
        if (!copy) {
            console::println("[fs] out of memory for initrd copy", FG);
        } else {
            util::memcpy(copy, tar, tarsize);

            const uint8_t *p = copy;
            size_t left = tarsize;

            while (left >= 512 && s_count < MAX_ENTRIES) {
                if (p[0] == 0) break; // two zero blocks = end
                // "ustar" magic at 257 (accept version suffix).
                if (p[257] != 'u' || p[258] != 's' || p[259] != 't' ||
                    p[260] != 'a' || p[261] != 'r')
                    break; // not a tar (or corrupt) -> stop
                uint64_t fsize = octal((const char *)p + 124, 12);
                uint64_t blocks = (fsize + 511) / 512;
                if (512 + blocks * 512 > left) break; // truncated
                char type = (char)p[156];
                if ((type == '0' || type == 0) && fsize > 0) {
                    Entry &e = s_entries[s_count++];
                    size_t n = 0;
                    while (n < 99 && p[n]) { e.name[n] = (char)p[n]; n++; }
                    e.name[n] = 0;
                    e.data = p + 512;
                    e.size = (size_t)fsize;
                }
                p += 512 + blocks * 512;
                left -= 512 + (size_t)(blocks * 512);
            }
            console::kprintf(FG, BG, "[fs] initrd: %u file(s) from boot module (%u KiB)\n",
                             (unsigned)s_count, (unsigned)(mod->size / 1024));
        }
    }
    // Prefer the persistent ATA volume over the read-only boot module; the
    // first mount wins, so the writable one goes first.
    fat32::mount_ata();
    if (!fat32::mounted()) {
        for (uint64_t i = 0; i < bi.module_count; i++) {
            if (!bi.modules[i]) continue;
            const uint8_t *img = module_data(*bi.modules[i], bi.hhdm);
            if (!img || is_ustar(img)) continue;
            if (fat32::mount_ramdisk(img, (size_t)bi.modules[i]->size)) break;
        }
        // Legacy COLOVOL only when the FAT32 volume is absent.
        disk_persistence_init();
    }
}

size_t count() {
    size_t wn = 0;
    for (auto &w : s_writable) if (w.used) wn++;
    // A writable slot named disk/... shadows its FAT32 twin: count once.
    size_t fat = fat32::count();
    for (size_t i = 0; i < fat32::count(); i++) {
        const char *n = nullptr;
        if (!fat32::stat(i, &n, nullptr) || !n) continue;
        for (auto &w : s_writable)
            if (w.used && !util::strcmp(n, w.name)) { fat--; break; }
    }
    return s_count + wn + fat + procfs::count();
}

Summary summary() {
    Summary s;
    s.initrd = s_count;
    for (auto &w : s_writable)
        if (w.used) { s.writable++; s.writable_bytes += w.size; }
    s.fat = fat32::count();
    s.proc = procfs::count();
    s.fat_mounted = fat32::mounted();
    s.fat_rw = fat32::writable();
    s.fat_bytes = fat32::volume_bytes();
    s.fat_free = fat32::free_bytes();
    return s;
}

bool stat(size_t i, const char **name_out, size_t *size_out) {
    if (i < s_count) {
        if (name_out) *name_out = s_entries[i].name;
        if (size_out) *size_out = s_entries[i].size;
        return true;
    }
    i -= s_count;
    for (auto &w : s_writable) {
        if (!w.used) continue;
        if (i == 0) {
            if (name_out) *name_out = w.name;
            if (size_out) *size_out = w.size;
            return true;
        }
        i--;
    }
    for (size_t k = 0; k < fat32::count(); k++) {
        const char *n = nullptr;
        size_t sz = 0;
        if (!fat32::stat(k, &n, &sz)) continue;
        bool shadowed = false;
        for (auto &w : s_writable)
            if (w.used && n && !util::strcmp(n, w.name)) { shadowed = true; break; }
        if (shadowed) continue;
        if (i == 0) {
            if (name_out) *name_out = n;
            if (size_out) *size_out = sz;
            return true;
        }
        i--;
    }
    return procfs::stat(i, name_out, size_out);
}

bool read(const char *path, const void **data_out, size_t *size_out) {
    if (!path) return false;
    while (*path == '/') path++; // accept "/bin/hello" and "bin/hello"
    for (auto &w : s_writable) {
        if (w.used && !util::strcmp(path, w.name)) {
            if (data_out) *data_out = w.data;
            if (size_out) *size_out = w.size;
            return true;
        }
    }
    for (size_t i = 0; i < s_count; i++) {
        if (!util::strcmp(path, s_entries[i].name)) {
            if (data_out) *data_out = s_entries[i].data;
            if (size_out) *size_out = s_entries[i].size;
            return true;
        }
    }
    if (fat32::read(path, data_out, size_out)) return true; // /disk files
    return procfs::read(path, data_out, size_out); // proc/version, ...
}

long writable_open(const char *path, bool create, bool truncate,
                   const uint8_t **data_out, size_t *size_out) {
    if (!path) return -14;
    while (*path == '/') path++;
    // FAT32-backed paths ("disk/..."): when the volume is writable the file
    // is staged into a writable RAM slot, and disk_persistence_flush() writes
    // staged slots back through the FAT32 driver.
    bool disk_path = path[0] == 'd' && path[1] == 'i' && path[2] == 's' &&
                     path[3] == 'k' && path[4] == '/';
    if (disk_path && fat32::writable()) {
        for (size_t i = 0; i < MAX_WRITABLE; i++) {
            if (s_writable[i].used && !util::strcmp(path, s_writable[i].name)) {
                if (truncate) s_writable[i].size = 0;
                if (data_out) *data_out = s_writable[i].data;
                if (size_out) *size_out = s_writable[i].size;
                return (long)i;
            }
        }
        if (!create && !fat32::read(path, nullptr, nullptr)) return -2;
        // Stage the current content.
        const void *src = nullptr;
        size_t src_size = 0;
        bool has_src = fat32::read(path, &src, &src_size);
        if (!has_src && !create) return -2;
        Writable *free_slot = nullptr;
        for (auto &w : s_writable) if (!w.used) { free_slot = &w; break; }
        if (!free_slot) return -28;
        if (src_size > WRITABLE_CAP) return -27;
        free_slot->data = (uint8_t *)heap::alloc(WRITABLE_CAP);
        if (!free_slot->data) return -12;
        util::memset(free_slot->data, 0, WRITABLE_CAP);
        if (src && src_size) util::memcpy(free_slot->data, src, src_size);
        size_t n = 0;
        while (n + 1 < sizeof(free_slot->name) && path[n])
            free_slot->name[n] = path[n], n++;
        free_slot->name[n] = 0;
        free_slot->used = true;
        free_slot->size = truncate ? 0 : src_size;
        if (data_out) *data_out = free_slot->data;
        if (size_out) *size_out = free_slot->size;
        return (long)(free_slot - s_writable);
    }
    // Plain writable area (root paths).
    Writable *free_slot = nullptr;
    for (size_t i = 0; i < MAX_WRITABLE; i++) {
        if (s_writable[i].used && !util::strcmp(path, s_writable[i].name)) {
            if (truncate) s_writable[i].size = 0;
            if (data_out) *data_out = s_writable[i].data;
            if (size_out) *size_out = s_writable[i].size;
            return (long)i;
        }
        if (!s_writable[i].used && !free_slot) free_slot = &s_writable[i];
    }
    if (!create) return -2;
    if (!free_slot) return -28;
    if (!parents_exist(path)) return -2;
    free_slot->data = (uint8_t *)heap::alloc(WRITABLE_CAP);
    if (!free_slot->data) return -12;
    util::memset(free_slot->data, 0, WRITABLE_CAP);
    size_t n = 0;
    while (n + 1 < sizeof(free_slot->name) && path[n])
        free_slot->name[n] = path[n], n++;
    if (!path[n]) {
        free_slot->name[n] = 0;
        free_slot->used = true;
        if (data_out) *data_out = free_slot->data;
        if (size_out) *size_out = 0;
        return (long)(free_slot - s_writable);
    }
    heap::free(free_slot->data); *free_slot = Writable{};
    return -36;
}

long writable_write(long id, size_t off, const void *src, size_t len, bool append,
                    size_t *new_off) {
    if (id < 0 || id >= (long)MAX_WRITABLE || !s_writable[id].used) return -9;
    Writable &w = s_writable[id];
    if (append) off = w.size;
    if (off > WRITABLE_CAP || len > WRITABLE_CAP - off) return -27;
    util::memcpy(w.data + off, src, len);
    if (off + len > w.size) w.size = off + len;
    if (new_off) *new_off = off + len;
    return (long)len;
}

bool writable_unlink(const char *path) {
    if (!path) return false;
    while (*path == '/') path++;
    if (path[0] == 'd' && path[1] == 'i' && path[2] == 's' && path[3] == 'k' &&
        path[4] == '/' && fat32::writable()) {
        bool overlay = false;
        for (auto &w : s_writable)
            if (w.used && !util::strcmp(path, w.name)) {
                heap::free(w.data); w = Writable{}; overlay = true;
            }
        if (!fat32::unlink(path)) return overlay;
        return true;
    }
    for (auto &w : s_writable) if (w.used && !util::strcmp(path, w.name)) {
        heap::free(w.data); w = Writable{}; return true;
    }
    return false;
}

bool writable_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return false;
    while (*old_path == '/') old_path++;
    while (*new_path == '/') new_path++;
    bool disk_old = old_path[0] == 'd' && old_path[1] == 'i' && old_path[2] == 's' &&
                    old_path[3] == 'k' && old_path[4] == '/';
    bool disk_new = new_path[0] == 'd' && new_path[1] == 'i' && new_path[2] == 's' &&
                    new_path[3] == 'k' && new_path[4] == '/';
    if (disk_old || disk_new) {
        if (!(disk_old && disk_new)) return false;
        if (fat32::writable()) return fat32::rename(old_path, new_path);
        return false;
    }
    for (auto &w : s_writable) if (w.used && !util::strcmp(old_path, w.name)) {
        size_t n = 0;
        if (!parents_exist(new_path)) return false;
        while (n + 1 < sizeof(w.name) && new_path[n])
            w.name[n] = new_path[n], n++;
        if (new_path[n]) return false;
        w.name[n] = 0; return true;
    }
    return false;
}

bool writable_mkdir(const char *path) {
    if (!path) return false;
    while (*path == '/') path++;
    if (path[0] == 'd' && path[1] == 'i' && path[2] == 's' && path[3] == 'k' &&
        path[4] == '/' && fat32::writable())
        return fat32::mkdir(path);
    if (!*path || is_dir_internal(path)) return false;
    for (auto &w : s_writable) if (w.used && !util::strcmp(path, w.name)) return false;
    if (!parents_exist(path)) return false;
    for (auto &d : s_dirs) if (!d.used) {
        size_t n = 0;
        while (n + 1 < sizeof(d.name) && path[n]) d.name[n] = path[n], n++;
        if (path[n]) return false;
        d.name[n] = 0; d.used = true; return true;
    }
    return false;
}

bool writable_rmdir(const char *path) {
    if (!path) return false;
    while (*path == '/') path++;
    if (path[0] == 'd' && path[1] == 'i' && path[2] == 's' && path[3] == 'k' &&
        path[4] == '/' && fat32::writable())
        return fat32::unlink(path); // empty dirs remove the same way
    for (auto &d : s_dirs) if (d.used && !util::strcmp(path, d.name)) {
        d = Dir{}; return true;
    }
    return false;
}

bool writable_is_dir(const char *path) {
    return is_dir_internal(path) || fat32::is_dir(path);
}

// --- optional persistent volume on the secondary IDE channel ---
// The volume is a plain raw LBA image attached as the master of the secondary
// channel (QEMU: -drive file=chocovol.img,if=ide,index=2,format=raw). Detection
// is defensive on purpose: with no disk attached the in-memory VFS keeps
// working and every write below is skipped, so a missing volume can never
// fault the kernel or block boot.
namespace {

constexpr uint16_t VOL_IO = 0x170;
constexpr uint8_t VOL_DRIVE = 0xE0; // secondary master
constexpr uint64_t VOL_MAGIC = 0x434F4C4F564F4C31ULL; // "COLOVOL1"
constexpr size_t VOL_FILES = 16, VOL_DIRS = 16;
constexpr uint64_t VOL_DATA_BASE = 64;
constexpr size_t VOL_SECS_PER_FILE = 32; // 16 KiB per persistent file

struct VolFileDesc {
    char name[100];
    uint32_t lba;
    uint32_t sectors;
    uint32_t size;
};
static_assert(sizeof(VolFileDesc) == 112, "VolFileDesc must pack");
struct VolDirDesc { char name[100]; };
static_assert(sizeof(VolDirDesc) == 100, "VolDirDesc must pack");

struct VolSuper {
    uint64_t magic;
    uint32_t nfiles;
    uint32_t ndirs;
    uint32_t seq;
    uint8_t pad[492];
};
static_assert(sizeof(VolSuper) == 512, "VolSuper must be one sector");

bool s_vol_ready = false;
uint16_t s_sec[256]{}; // one-sector scratch buffer (512 bytes)

bool vol_bsy_clear() {
    for (int i = 0; i < 200000; i++)
        if (!(cpu::inb(VOL_IO + 7) & 0x80)) return true;
    return false;
}

bool vol_drq() {
    for (int i = 0; i < 200000; i++) {
        uint8_t st = cpu::inb(VOL_IO + 7);
        if (st & 0x80) continue; // BSY
        if (st & 0x08) return true; // DRQ
        if (st & 0x01) return false; // ERR
    }
    return false;
}

bool vol_rw(uint64_t lba, uint16_t sectors, uint16_t *buf, bool write) {
    if (lba > 0x0FFFFFFFULL || !vol_bsy_clear()) return false;
    cpu::outb(VOL_IO + 6, (uint8_t)(VOL_DRIVE | ((lba >> 24) & 0x0F)));
    cpu::outb(VOL_IO + 2, (uint8_t)(sectors & 0xFF));
    cpu::outb(VOL_IO + 3, (uint8_t)(lba & 0xFF));
    cpu::outb(VOL_IO + 4, (uint8_t)((lba >> 8) & 0xFF));
    cpu::outb(VOL_IO + 5, (uint8_t)((lba >> 16) & 0xFF));
    cpu::outb(VOL_IO + 7, write ? 0x30 : 0x20);
    for (uint16_t s = 0; s < sectors; s++) {
        if (!vol_drq()) return false;
        for (int i = 0; i < 256; i++) {
            if (write) {
                cpu::outb(VOL_IO, (uint8_t)(buf[s * 256 + i] & 0xFF));
                cpu::outb(VOL_IO, (uint8_t)(buf[s * 256 + i] >> 8));
            } else {
                uint8_t lo = cpu::inb(VOL_IO), hi = cpu::inb(VOL_IO);
                buf[s * 256 + i] = (uint16_t)(lo | (hi << 8));
            }
        }
    }
    return true;
}

bool vol_identify() {
    if (!vol_bsy_clear()) {
        console::println("[vol] identify: bsy clear failed", FG);
        return false;
    }
    cpu::outb(VOL_IO + 6, VOL_DRIVE);
    cpu::outb(VOL_IO + 2, 0);
    cpu::outb(VOL_IO + 3, 0);
    cpu::outb(VOL_IO + 4, 0);
    cpu::outb(VOL_IO + 5, 0);
    cpu::outb(VOL_IO + 7, 0xEC);
    uint8_t st = cpu::inb(VOL_IO + 7);
    console::kprintf(FG, BG, "[vol] identify status=0x%x\n", (unsigned)st);
    if (!st) return false;
    if (!vol_bsy_clear()) {
        console::println("[vol] identify: bsy clear after cmd failed", FG);
        return false;
    }
    for (int i = 0; i < 256; i++) {
        if (!vol_drq()) {
            uint8_t st2 = cpu::inb(VOL_IO + 7);
            console::kprintf(FG, BG, "[vol] identify: drq failed at %d, status=0x%x\n", i, (unsigned)st2);
            return false;
        }
        (void)cpu::inb(VOL_IO);
        (void)cpu::inb(VOL_IO);
    }
    console::println("[vol] identify: OK", FG);
    return true;
}

void copy_name(char (&dst)[100], const char *src) {
    size_t n = 0;
    while (n + 1 < 100 && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

} // namespace

long disk_persistence_init() {
    if (s_vol_ready) return 0;
    // Diagnostic: read the status register to see if the drive responds.
    uint8_t st = cpu::inb(VOL_IO + 7);
    console::kprintf(FG, BG, "[vol] status=0x%x\n", (unsigned)st);
    if (!vol_identify()) {
        console::println("[vol] no disk on the secondary channel: memory-only VFS", FG);
        return -1;
    }
    VolSuper super{};
    if (!vol_rw(0, 1, s_sec, false)) return -5;
    util::memcpy(&super, s_sec, sizeof(super));
    if (super.magic != VOL_MAGIC) {
        util::memset(s_sec, 0, sizeof(s_sec));
        super = VolSuper{};
        super.magic = VOL_MAGIC;
        util::memcpy(s_sec, &super, sizeof(super));
        if (!vol_rw(0, 1, s_sec, true)) return -5;
        console::kprintf(OK, BG, "[vol] formatted persistent volume (%u files, %u dirs)\n",
                         (unsigned)VOL_FILES, (unsigned)VOL_DIRS);
        s_vol_ready = true;
        return 0;
    }
    // Load persisted file descriptors, then their payloads.
    for (uint64_t sec = 1; sec <= 4; sec++) {
        if (!vol_rw(sec, 1, s_sec, false)) return -5;
        for (int k = 0; k < 4; k++) {
            VolFileDesc d{};
            util::memcpy(&d, (uint8_t *)s_sec + k * sizeof(d), sizeof(d));
            if (!d.name[0] || !d.size || d.sectors > VOL_SECS_PER_FILE) continue;
            Writable *slot = nullptr;
            for (auto &w : s_writable) if (!w.used) { slot = &w; break; }
            if (!slot) break;
            slot->data = (uint8_t *)heap::alloc(WRITABLE_CAP);
            if (!slot->data) continue;
            util::memset(slot->data, 0, WRITABLE_CAP);
            size_t got = 0;
            for (uint32_t s = 0; s < d.sectors; s++) {
                if (!vol_rw(d.lba + s, 1, s_sec, false)) break;
                size_t chunk = d.size - got;
                if (chunk > 512) chunk = 512;
                util::memcpy(slot->data + got, s_sec, chunk);
                got += chunk;
            }
            if (got) {
                copy_name(slot->name, d.name);
                slot->size = got;
                slot->used = true;
            }
        }
    }
    // Load persisted directory descriptors.
    for (uint64_t sec = 5; sec <= 8; sec++) {
        if (!vol_rw(sec, 1, s_sec, false)) return -5;
        for (int k = 0; k < 5; k++) {
            VolDirDesc d{};
            util::memcpy(&d, (uint8_t *)s_sec + k * sizeof(d), sizeof(d));
            if (!d.name[0]) continue;
            for (auto &dir : s_dirs) if (!dir.used) {
                copy_name(dir.name, d.name);
                dir.used = true;
                break;
            }
        }
    }
    console::println("[vol] mounted persistent volume", OK);
    s_vol_ready = true;
    return 0;
}

void disk_persistence_flush() {
    // FAT32-backed volume: writable slots named disk/... are write-cache
    // entries; flush pushes them through the driver immediately.
    if (fat32::writable()) {
        for (auto &w : s_writable) {
            if (!w.used) continue;
            if (!(w.name[0] == 'd' && w.name[1] == 'i' && w.name[2] == 's' &&
                  w.name[3] == 'k' && w.name[4] == '/'))
                continue;
            fat32::write_file(w.name, w.data, w.size);
        }
        return;
    }
    if (!s_vol_ready) return; // memory-only: nothing to flush
    VolSuper super{};
    super.magic = VOL_MAGIC;
    uint32_t nfiles = 0, ndirs = 0;
    uint32_t cursor = VOL_DATA_BASE;
    // File descriptors live 4 per sector starting at LBA 1.
    for (uint64_t sec = 1; sec <= 4; sec++) {
        util::memset(s_sec, 0, sizeof(s_sec));
        if (!vol_rw(sec, 1, s_sec, true)) return;
    }
    for (uint64_t sec = 5; sec <= 8; sec++) {
        util::memset(s_sec, 0, sizeof(s_sec));
        if (!vol_rw(sec, 1, s_sec, true)) return;
    }
    for (auto &w : s_writable) {
        if (!w.used) continue;
        if (nfiles >= VOL_FILES) continue;
        if (w.size > VOL_SECS_PER_FILE * 512) continue; // too big to persist
        VolFileDesc d{};
        copy_name(d.name, w.name);
        d.lba = cursor;
        d.sectors = (uint32_t)((w.size + 511) / 512);
        d.size = (uint32_t)w.size;
        // Payload first: a power loss never leaves a descriptor without data.
        for (uint32_t s = 0; s < d.sectors; s++) {
            util::memset(s_sec, 0, sizeof(s_sec));
            size_t chunk = w.size - s * 512;
            if (chunk > 512) chunk = 512;
            util::memcpy(s_sec, w.data + s * 512, chunk);
            if (!vol_rw(d.lba + s, 1, s_sec, true)) return;
        }
        uint64_t target = nfiles / 4 + 1;
        unsigned slot = nfiles % 4;
        if (!vol_rw(target, 1, s_sec, false)) return;
        util::memcpy((uint8_t *)s_sec + slot * sizeof(d), &d, sizeof(d));
        if (!vol_rw(target, 1, s_sec, true)) return;
        cursor += d.sectors;
        nfiles++;
    }
    for (auto &dir : s_dirs) {
        if (!dir.used) continue;
        if (ndirs >= VOL_DIRS) continue;
        VolDirDesc d{};
        copy_name(d.name, dir.name);
        uint64_t target = ndirs / 5 + 5;
        unsigned slot = ndirs % 5;
        if (!vol_rw(target, 1, s_sec, false)) return;
        util::memcpy((uint8_t *)s_sec + slot * sizeof(d), &d, sizeof(d));
        if (!vol_rw(target, 1, s_sec, true)) return;
        ndirs++;
    }
    super.nfiles = nfiles;
    super.ndirs = ndirs;
    super.seq++;
    util::memset(s_sec, 0, sizeof(s_sec));
    util::memcpy(s_sec, &super, sizeof(super));
    vol_rw(0, 1, s_sec, true);
}

} // namespace choco::fs

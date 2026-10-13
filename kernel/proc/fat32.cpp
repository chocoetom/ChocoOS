// FAT32 driver: ramdisk (read-only) or ATA secondary volume (read/write).
//   BPB -> geometry -> FAT cluster chain walk -> 8.3 + LFN directory entries.
// On mount the whole tree is flattened into s_entries[] with full paths
// ("disk/..."), which is the same convention procfs/initrd already use. That
// keeps the fs:: surface untouched (zero new syscalls).
#include <choco/fat32.hpp>
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/mem.hpp>
#include <choco/util.hpp>

namespace choco::fs::fat32 {
namespace {

using choco::console::BG;
using choco::console::FG;
using choco::console::OK;
using choco::console::ERR;

constexpr size_t MAX_ENTRIES = 64;
constexpr size_t MAX_PATH = 160;

struct Slot {
    char path[MAX_PATH];
    size_t size;
    bool is_dir;
    const uint8_t *data;
};

Slot s_entries[MAX_ENTRIES]{};
size_t s_count = 0;

uint8_t *s_img = nullptr;    // module view or whole ATA volume in RAM
size_t s_img_size = 0;
bool s_writable = false;     // true when backed by the ATA volume
uint32_t s_nfats = 2;

uint32_t s_bps = 512;        // bytes per sector (512 only)
uint32_t s_spc = 1;          // sectors per cluster
uint32_t s_fat_size = 0;     // sectors per FAT
uint32_t s_reserved = 0;     // reserved sectors before FAT1
uint32_t s_data_start = 0;   // first data sector
uint32_t s_cluster_count = 0;
uint32_t s_root_cluster = 2;
uint8_t *s_fat = nullptr;

bool s_mounted = false;

inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (v >> 24);
}

bool in_bounds(uint64_t off, uint64_t len) {
    return off + len >= off && off + len <= s_img_size;
}

inline bool cluster_eoc(uint32_t c) { return c >= 0x0FFFFFF8; }
inline bool cluster_live(uint32_t c) { return c >= 0x0FFFFFF8 || c == 0x0FFFFFF7; }
inline bool cluster_valid(uint32_t c) { return c >= 2 && c < s_cluster_count + 2; }

uint64_t cluster_offset(uint32_t cluster) {
    return ((uint64_t)s_data_start + (uint64_t)(cluster - 2) * s_spc) * s_bps;
}

uint32_t fat_next(uint32_t cluster) {
    uint64_t off = (uint64_t)cluster * 4;
    if (off + 4 > (uint64_t)s_fat_size * s_bps) return 0x0FFFFFFF;
    return rd32(s_fat + off) & 0x0FFFFFFF;
}

void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (n + 1 < cap && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

void append_slash(char *dst, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap && dst[n]) n++;
    if (n + 1 < cap) { dst[n] = '/'; dst[n + 1] = 0; }
}

bool join_path(char *dst, size_t cap, const char *prefix, const char *name) {
    size_t n = 0;
    while (n + 1 < cap && prefix[n]) { dst[n] = prefix[n]; n++; }
    for (size_t i = 0; n + 1 < cap && name[i]; i++) dst[n++] = name[i];
    dst[n] = 0;
    return n > 0;
}

// --- ATA backend: scans all four legacy IDE positions (primary/secondary,
// master/slave) and picks the first real ATA disk. CD-ROMs (ATAPI) and empty
// ports are skipped, so the QEMU/PC wiring doesn't matter.
namespace ata {

struct Chan {
    uint16_t io;     // 0x1F0 primary, 0x170 secondary
    uint8_t  drive;  // 0xE0 master, 0xF0 slave
};

// Chosen channel at detect time.
Chan g_chan{0, 0};
bool g_detected = false;
uint32_t g_sectors = 0;      // size reported by IDENTIFY

// Traffic counters for /proc/diskstats. Single-core, thread context only.
uint64_t g_reads = 0, g_writes = 0, g_read_sectors = 0, g_write_sectors = 0;
uint64_t g_errors = 0;

bool bsy_clear(uint16_t io) {
    for (int i = 0; i < 200000; i++)
        if (!(choco::cpu::inb(io + 7) & 0x80)) return true;
    return false;
}

bool drq(uint16_t io) {
    for (int i = 0; i < 200000; i++) {
        uint8_t st = choco::cpu::inb(io + 7);
        if (st & 0x80) continue; // BSY
        if (st & 0x08) return true; // DRQ
        if (st & 0x01) return false; // ERR
    }
    return false;
}

// IDENTIFY at one position. Returns the sector count on an ATA disk, 0
// otherwise. ATAPI devices answer IDENTIFY PACKET (0xA1), not IDENTIFY, so
// ERR after 0xEC filters CD-ROMs automatically.
uint32_t detect_at(uint16_t io, uint8_t drive) {
    if (!bsy_clear(io)) return 0;
    choco::cpu::outb(io + 6, drive);
    choco::cpu::outb(io + 2, 0);
    choco::cpu::outb(io + 3, 0);
    choco::cpu::outb(io + 4, 0);
    choco::cpu::outb(io + 5, 0);
    choco::cpu::outb(io + 7, 0xEC); // IDENTIFY DEVICE
    // Post-command settle: real controllers and emulators both need it.
    for (int i = 0; i < 1000; i++) choco::cpu::inb(io + 7);
    uint8_t st = choco::cpu::inb(io + 7);
    if (!st || st == 0xFF) return 0; // floating bus = nothing attached
    if (st & 0x01) return 0;         // ERR => ATAPI or absent
    if (!bsy_clear(io)) return 0;
    if (!drq(io)) return 0;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = choco::cpu::inw(io);
    return (uint32_t)id[60] | ((uint32_t)id[61] << 16);
}

bool detect(uint32_t *sectors_out) {
    static const struct { uint16_t io; uint8_t drive; } k[4] = {
        {0x1F0, 0xE0}, // primary master   (typical hard disk)
        {0x1F0, 0xF0}, // primary slave
        {0x170, 0xE0}, // secondary master (typical CD-ROM, HD ok too)
        {0x170, 0xF0}, // secondary slave
    };
    for (int i = 0; i < 4; i++) {
        uint32_t sectors = detect_at(k[i].io, k[i].drive);
        if (sectors) {
            g_chan.io = k[i].io;
            g_chan.drive = k[i].drive;
            g_detected = true;
            g_sectors = sectors;
            if (sectors_out) *sectors_out = sectors;
            choco::console::kprintf(choco::console::OK, choco::console::BG,
                                    "[ata] disk on 0x%x drive 0x%x (%u sectors)\n",
                                    (unsigned)k[i].io, (unsigned)k[i].drive,
                                    (unsigned)sectors);
            return true;
        }
    }
    return false;
}

bool rw(uint64_t lba, uint16_t count, uint16_t *buf, bool write) {
    if (!g_detected) return false;
    uint16_t io = g_chan.io;
    if (lba > 0x0FFFFFFFULL || !bsy_clear(io)) return false;
    choco::cpu::outb(io + 6, (uint8_t)(g_chan.drive | ((lba >> 24) & 0x0F)));
    // 400 ns settle after the drive/head select before the rest of the command
    // block (ATA spec); a missing delay shows up as intermittent ERR on long
    // PIO bursts.
    (void)choco::cpu::inb(io + 7);
    (void)choco::cpu::inb(io + 7);
    (void)choco::cpu::inb(io + 7);
    (void)choco::cpu::inb(io + 7);
    choco::cpu::outb(io + 2, (uint8_t)(count & 0xFF));
    choco::cpu::outb(io + 3, (uint8_t)(lba & 0xFF));
    choco::cpu::outb(io + 4, (uint8_t)((lba >> 8) & 0xFF));
    choco::cpu::outb(io + 5, (uint8_t)((lba >> 16) & 0xFF));
    choco::cpu::outb(io + 7, write ? 0x30 : 0x20);
    for (uint16_t s = 0; s < count; s++) {
        if (!drq(io)) {
            g_errors++;
            return false;
        }
        for (int i = 0; i < 256; i++) {
            if (write) choco::cpu::outw(io, buf[s * 256 + i]);
            else buf[s * 256 + i] = choco::cpu::inw(io);
        }
    }
    if (write) {
        g_writes++;
        g_write_sectors += count;
    } else {
        g_reads++;
        g_read_sectors += count;
    }
    return true;
}

} // namespace ata

// --- write-through flush: push a changed image range to the ATA volume ---
void flush_range(uint64_t off, uint64_t len) {
    if (!s_writable || !s_img || len == 0) return;
    uint64_t lba0 = off / 512;
    uint64_t end = off + len;
    uint64_t lba1 = end / 512;
    if (end % 512 == 0) lba1 = end / 512 - 1;
    for (uint64_t lba = lba0; lba <= lba1; lba++)
        ata::rw((uint32_t)lba, 1, (uint16_t *)(s_img + lba * 512), true);
}

// Write one FAT entry (keeps the reserved high nibble), mirrored across
// every FAT copy, and flushed to the volume.
void fat_set(uint32_t cluster, uint32_t value) {
    uint64_t eoff = (uint64_t)cluster * 4;
    if (eoff + 4 > (uint64_t)s_fat_size * s_bps) return;
    uint64_t fat_off = (uint64_t)s_reserved * s_bps;
    for (uint32_t k = 0; k < s_nfats; k++) {
        uint64_t off = fat_off + (uint64_t)k * s_fat_size * s_bps + eoff;
        if (!in_bounds(off, 4)) break;
        uint32_t old = rd32(s_img + off);
        wr32(s_img + off, (value & 0x0FFFFFFF) | (s_writable ? (old & 0xF0000000) : 0));
        flush_range(off, 4);
    }
}

uint32_t fat_alloc() {
    for (uint32_t c = 3; c < s_cluster_count + 2; c++) {
        if (fat_next(c) == 0) { fat_set(c, 0x0FFFFFFF); return c; }
    }
    return 0;
}

void fat_free_chain(uint32_t first) {
    uint32_t c = first;
    uint32_t guard = 0;
    while (cluster_valid(c) && !cluster_live(c) && ++guard < 65536) {
        uint32_t next = fat_next(c);
        fat_set(c, 0);
        c = next;
    }
}

// Zero a cluster in the image and on the volume.
void cluster_zero(uint32_t cluster) {
    uint64_t off = cluster_offset(cluster);
    size_t len = (size_t)s_spc * s_bps;
    if (!in_bounds(off, len)) return;
    util::memset(s_img + off, 0, len);
    flush_range(off, len);
}

char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = to_upper(*a), cb = to_upper(*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

// 8.3 name ("README.TXT"), trimmed of padding spaces. The NT reserved byte
// (offset 12) carries the lowercase flags: bit 3 = base, bit 4 = extension.
void build_short_name(const uint8_t *de, char *out, size_t cap) {
    uint8_t nt = de[12];
    bool lower_base = (nt & 0x08) != 0;
    bool lower_ext = (nt & 0x10) != 0;
    char base[9]; int bn = 0;
    for (int i = 0; i < 8; i++) {
        char c = (char)de[i];
        if (c == ' ') break;
        if (c) {
            if (lower_base && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            base[bn++] = c;
        }
    }
    base[bn] = 0;
    char ext[4]; int en = 0;
    for (int i = 8; i < 11; i++) {
        char c = (char)de[i];
        if (c == ' ') break;
        if (c) {
            if (lower_ext && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            ext[en++] = c;
        }
    }
    ext[en] = 0;
    size_t n = 0;
    for (int i = 0; base[i] && n + 1 < cap; i++) out[n++] = base[i];
    if (en) {
        if (n + 1 < cap) out[n++] = '.';
        for (int i = 0; ext[i] && n + 1 < cap; i++) out[n++] = ext[i];
    }
    out[n] = 0;
}

// One LFN slot (attr 0x0F) carries 13 UTF-16 chars. Slots are stored in
// reverse order: the entry tagged 0x40 is the last fragment and comes first.
void collect_lfn(const uint8_t *de, char *lfn) {
    if (de[0] & 0x40) util::memset(lfn, 0, 260);
    int seq = de[0] & 0x1F;
    if (seq < 1) return;
    int base = (seq - 1) * 13;
    if (base + 13 > 259) return;
    static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int i = 0; i < 13; i++) {
        uint16_t c = rd16(de + offs[i]);
        if (c == 0 || c == 0xFFFF) break;
        lfn[base + i] = (c < 128) ? (char)c : '?';
    }
}

// Follow the cluster chain from `first` and copy `size` bytes into the heap.
const uint8_t *read_chain(uint32_t first, uint32_t size) {
    if (size == 0) return s_img; // non-null placeholder for an empty file
    uint8_t *buf = (uint8_t *)heap::alloc(size);
    if (!buf) return nullptr;
    size_t got = 0;
    uint32_t cl = first;
    uint32_t guard = 0;
    while (got < size && cluster_valid(cl) && !cluster_eoc(cl)) {
        if (++guard > 65536) break;
        uint64_t base = cluster_offset(cl);
        size_t chunk = (size_t)s_spc * s_bps;
        if (chunk > size - got) chunk = size - got;
        if (!in_bounds(base, chunk)) break;
        util::memcpy(buf + got, s_img + base, chunk);
        got += chunk;
        cl = fat_next(cl);
    }
    if (got < size) util::memset(buf + got, 0, size - got);
    return buf;
}

void flatten_dir(uint32_t start_cluster, const char *prefix, int depth) {
    char lfn[260];
    util::memset(lfn, 0, sizeof(lfn));
    bool lfn_used = false;

    uint32_t cl = start_cluster;
    uint32_t guard = 0;
    while (cluster_valid(cl) && !cluster_eoc(cl) && s_count < MAX_ENTRIES) {
        if (++guard > 4096) break; // paranoid guard against a corrupt chain
        uint64_t base = cluster_offset(cl);
        for (uint32_t s = 0; s < s_spc; s++) {
            uint64_t off = base + (uint64_t)s * s_bps;
            if (!in_bounds(off, s_bps)) return;
            const uint8_t *sec = s_img + off;
            for (uint32_t e = 0; e + 32 <= s_bps; e += 32) {
                const uint8_t *de = sec + e;
                if (de[0] == 0x00) return;   // end of directory
                if (de[0] == 0xE5) continue; // deleted
                uint8_t attr = de[11];
                if (attr == 0x0F) { collect_lfn(de, lfn); lfn_used = true; continue; }
                if ((attr & 0x08) && !(attr & 0x10)) continue; // volume label

                char name[260];
                if (lfn_used) {
                    size_t n = 0;
                    while (n + 1 < sizeof(name) && lfn[n]) { name[n] = lfn[n]; n++; }
                    name[n] = 0;
                } else {
                    build_short_name(de, name, sizeof(name));
                }
                lfn_used = false;
                if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                    continue; // "." and ".."

                bool is_directory = (attr & 0x10) != 0;
                uint32_t first = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
                uint32_t fsize = rd32(de + 28);

                char path[MAX_PATH];
                if (!join_path(path, sizeof(path), prefix, name)) continue;

                if (is_directory) {
                    if (s_count < MAX_ENTRIES) {
                        Slot &slot = s_entries[s_count++];
                        copy_str(slot.path, sizeof(slot.path), path);
                        append_slash(slot.path, sizeof(slot.path));
                        slot.size = 0;
                        slot.is_dir = true;
                        slot.data = nullptr;
                    }
                    if (depth < 8 && cluster_valid(first)) {
                        char sub[MAX_PATH];
                        copy_str(sub, sizeof(sub), path);
                        append_slash(sub, sizeof(sub));
                        flatten_dir(first, sub, depth + 1);
                    }
                } else {
                    const uint8_t *data = read_chain(first, fsize);
                    if (!data) continue;
                    if (s_count < MAX_ENTRIES) {
                        Slot &slot = s_entries[s_count++];
                        copy_str(slot.path, sizeof(slot.path), path);
                        slot.size = fsize;
                        slot.is_dir = false;
                        slot.data = data;
                    }
                }
            }
        }
        cl = fat_next(cl);
    }
}

// True when two paths are equal ignoring a single trailing '/'.
bool path_eq(const char *a, const char *b) {
    while (*a == '/') a++;
    while (*b == '/') b++;
    size_t la = 0, lb = 0;
    while (a[la]) la++;
    while (b[lb]) lb++;
    if (la > 0 && a[la - 1] == '/') la--;
    if (lb > 0 && b[lb - 1] == '/') lb--;
    if (la != lb) return false;
    for (size_t i = 0; i < la; i++) if (a[i] != b[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Write path (ATA-backed volumes only). Everything mutates the RAM image and
// flushes the dirtied sectors straight back via flush_range().
// ---------------------------------------------------------------------------

void free_entry_data() {
    for (size_t i = 0; i < s_count; i++) {
        if (!s_entries[i].is_dir && s_entries[i].data && s_entries[i].size > 0)
            heap::free((void *)s_entries[i].data);
    }
}

void rescan() {
    free_entry_data();
    s_count = 0;
    flatten_dir(s_root_cluster, "disk/", 0);
}

// Split "disk/docs/notes.txt" -> parent components after "disk/", and copy
// the final component into `name`. Root paths give an empty component count.
// Returns parent cluster, or 0 on a missing parent.
uint32_t resolve_parent(const char *path, char *name, size_t name_cap) {
    while (*path == '/') path++;
    const char *p = path;
    if (p[0] == 'd' && p[1] == 'i' && p[2] == 's' && p[3] == 'k' && p[4] == '/')
        p += 5;
    else if (!name_cmp(p, "disk") && !p[4])
        return 0; // the volume root has no parent

    // Final component
    const char *last = p;
    const char *last_slash = nullptr;
    for (const char *q = p; *q; q++) if (*q == '/') last_slash = q;
    const char *base = last_slash ? last_slash + 1 : p;
    if (!base[0]) return 0;
    size_t n = 0;
    while (base[n] && n + 1 < name_cap) { name[n] = base[n]; n++; }
    name[n] = 0;

    uint32_t cl = s_root_cluster;
    const char *cur = p;
    while (last_slash && cur < last_slash) {
        const char *end = cur;
        while (*end && *end != '/') end++;
        if (end == cur) { cur++; continue; }
        char comp[32];
        size_t m = 0;
        while (cur + m < end && m + 1 < sizeof(comp)) { comp[m] = cur[m]; m++; }
        comp[m] = 0;
        (void)last;
        // Find `comp` inside directory `cl`; keep only if it is a directory.
        bool found = false;
        uint32_t dcl = cl;
        uint32_t guard = 0;
        while (cluster_valid(dcl) && !cluster_eoc(dcl) && ++guard < 4096) {
            uint64_t base = cluster_offset(dcl);
            for (uint32_t s = 0; s < s_spc && !found; s++) {
                uint64_t off = base + (uint64_t)s * s_bps;
                if (!in_bounds(off, s_bps)) break;
                const uint8_t *sec = s_img + off;
                for (uint32_t e = 0; e + 32 <= s_bps && !found; e += 32) {
                    const uint8_t *de = sec + e;
                    if (!de[0]) break;
                    if (de[0] == 0xE5) continue;
                    uint8_t attr = de[11];
                    if (attr == 0x0F) continue;      // LFN slot
                    if (attr & 0x08) continue;       // volume label
                    if (!(attr & 0x10)) continue;    // must be a directory
                    char sn[12];
                    build_short_name(de, sn, sizeof(sn));
                    if (name_cmp(sn, comp)) continue;
                    dcl = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
                    found = true;
                }
            }
            cl = dcl;
            dcl = fat_next(dcl);
        }
        if (!found) return 0;
        cur = end;
    }
    return cl;
}

// Locate `name` inside directory `dir_cluster`. Returns the absolute image
// offset of the short entry, or 0 when absent. LFNs preceding a short entry
// count toward it for deletion.
bool find_in_dir(uint32_t dir_cluster, const char *name, uint64_t *off_out,
                 uint64_t *lfn_from_out) {
    char lfn[260]{};
    bool lfn_used = false;
    uint32_t cl = dir_cluster;
    uint32_t guard = 0;
    while (cluster_valid(cl) && !cluster_eoc(cl) && ++guard < 4096) {
        uint64_t base = cluster_offset(cl);
        for (uint32_t s = 0; s < s_spc; s++) {
            uint64_t off = base + (uint64_t)s * s_bps;
            if (!in_bounds(off, s_bps)) return false;
            const uint8_t *sec = s_img + off;
            for (uint32_t e = 0; e + 32 <= s_bps; e += 32) {
                const uint8_t *de = sec + e;
                if (!de[0]) return false;
                if (de[0] == 0xE5) { lfn_used = false; continue; }
                uint8_t attr = de[11];
                if (attr == 0x0F) { collect_lfn(de, lfn); lfn_used = true; continue; }
                if (attr & 0x08) { lfn_used = false; continue; }
                const char *cand;
                char sn[12];
                if (lfn_used) cand = lfn;
                else { build_short_name(de, sn, sizeof(sn)); cand = sn; }
                bool hit = (name_cmp(cand, name) == 0);
                if (hit) {
                    if (off_out) *off_out = off + e;
                    if (lfn_from_out) {
                        // Where the LFN run starts: walk back while the
                        // previous slot has attr 0x0F in the same chain walk.
                        *lfn_from_out = off + e;
                        // The caller that deletes walks the chain again with
                        // lfn tracking; keeping this simple is deliberate.
                    }
                    return true;
                }
                lfn_used = false;
            }
        }
        cl = fat_next(cl);
    }
    return false;
}

// 8.3 validity: name[.ext], 1-8 uppercase-safe base chars, ext <= 3.
bool fits_83(const char *name) {
    int base = 0, ext = 0;
    bool dot = false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') { if (dot) return false; dot = true; continue; }
        if (c == ' ') return false;
        if (dot) { if (++ext > 3) return false; }
        else if (++base > 8) return false;
    }
    return base > 0;
}

bool has_lower_range(const char *s, int n) {
    for (int i = 0; i < n && s[i]; i++) if (s[i] >= 'a' && s[i] <= 'z') return true;
    return false;
}

// Build the padded 11-byte 8.3 field. Also computes the NT lowercase flags.
int make_short(const char *name, uint8_t *out11) {
    util::memset(out11, ' ', 11);
    int i = 0; // source
    int b = 0; // base fill
    int e = 0; // ext fill
    bool dot = false;
    char stem[13];
    int sn = 0;
    for (const char *p = name; *p && sn < 12; p++) stem[sn++] = *p;
    stem[sn] = 0;
    int nt = 0;
    for (; stem[i] && i < sn; i++) {
        char c = stem[i];
        if (c == '.') { dot = true; continue; }
        if (!dot) { if (b < 8) out11[b++] = (uint8_t)to_upper(c); }
        else if (e < 3) out11[8 + e++] = (uint8_t)to_upper(c);
    }
    if (has_lower_range(stem, 8)) nt |= 0x08;
    const char *ext = nullptr;
    for (int k = sn - 1; k >= 0; k--) if (stem[k] == '.') { ext = stem + k + 1; break; }
    if (ext && has_lower_range(ext, 3)) nt |= 0x10;
    return nt;
}

uint8_t lfn_checksum(const uint8_t *short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    return sum;
}

// Find `slots` consecutive free 32-byte entries inside `dir_cluster`. Extends
// the chain (allocating + zeroing a cluster) when the directory is full.
// Returns the absolute image offset of the first free slot, or 0.
uint64_t dir_find_free(uint32_t dir_cluster, int slots) {
    uint32_t cl = dir_cluster;
    uint32_t prev = 0;
    uint32_t guard = 0;
    int run = 0;
    uint64_t run_start = 0;
    uint64_t run_cluster = 0;
    (void)run_cluster;
    while (cluster_valid(cl) && !cluster_eoc(cl) && ++guard < 4096) {
        uint64_t base = cluster_offset(cl);
        for (uint32_t s = 0; s < s_spc; s++) {
            uint64_t off = base + (uint64_t)s * s_bps;
            if (!in_bounds(off, s_bps)) return 0;
            for (uint32_t e = 0; e + 32 <= s_bps; e += 32) {
                uint8_t b0 = s_img[off + e];
                if (b0 == 0x00 || b0 == 0xE5) {
                    if (run == 0) run_start = off + e;
                    if (++run >= slots) return run_start;
                } else {
                    run = 0;
                }
            }
        }
        prev = cl;
        cl = fat_next(cl);
    }
    // Directory is full: chain a fresh, zeroed cluster onto the tail.
    uint32_t fresh = fat_alloc();
    if (!fresh) return 0;
    if (prev) fat_set(prev, fresh);
    else {
        // Single-cluster dir (cluster 2 root dirs never extend from here,
        // but resolve_parent may yield it; treat the same).
        fat_set(dir_cluster, fresh);
    }
    cluster_zero(fresh);
    return cluster_offset(fresh);
}

// Write an LFN run + the short entry starting at absolute offset `off`.
// `slots` = 1 + LFN slot count.
void dir_write_entry(uint64_t off, const char *name, uint8_t attr,
                     uint32_t first_cluster, uint32_t size, int slots) {
    uint8_t sn[11];
    int nt = make_short(name, sn);
    int lfn_slots = slots - 1;

    if (lfn_slots > 0) {
        uint8_t sum = lfn_checksum(sn);
        // Slots are stored in reverse: the fragment tagged 0x40 first.
        int len = 0;
        while (name[len] && len < 255) len++;
        for (int k = lfn_slots - 1; k >= 0; k--) {
            uint8_t *de = s_img + off + (uint64_t)(lfn_slots - 1 - k) * 32;
            util::memset(de, 0xFF, 32);
            de[0] = (uint8_t)((k + 1) | ((k == lfn_slots - 1) ? 0x40 : 0));
            de[11] = 0x0F;
            de[13] = sum;
            static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int i = 0; i < 13; i++) {
                int idx = k * 13 + i;
                uint16_t c = (idx < len) ? (uint8_t)name[idx] : (uint16_t)(idx == len ? 0 : 0xFFFF);
                wr16(de + offs[i], c);
            }
        }
    }
    uint8_t *de = s_img + off + (uint64_t)lfn_slots * 32;
    util::memcpy(de, sn, 11);
    de[11] = attr;
    de[12] = (uint8_t)nt;
    util::memset(de + 13, 0, 7);
    wr16(de + 20, (uint16_t)(first_cluster >> 16));
    util::memset(de + 22, 0, 4);
    wr16(de + 26, (uint16_t)(first_cluster & 0xFFFF));
    wr32(de + 28, size);
}

// Slots an entry of `name` would occupy (short + LFN fragments).
int name_slots_total(const char *name) {
    if (fits_83(name)) return 1;
    size_t n = 0;
    while (name[n]) n++;
    return 1 + (int)((n + 12) / 13);
}

// Snapshot of an existing entry for copy-on-rename.
struct EntSnap {
    uint8_t attr;
    uint32_t first;
    uint32_t size;
};

bool lookup_full(const char *path, uint64_t *off_out, EntSnap *snap) {
    char name[MAX_PATH];
    uint32_t parent = resolve_parent(path, name, sizeof(name));
    if (!parent) return false;
    uint64_t off = 0;
    if (!find_in_dir(parent, name, &off, nullptr)) return false;
    if (off_out) *off_out = off;
    if (snap) {
        const uint8_t *de = s_img + off;
        snap->attr = de[11];
        snap->first = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
        snap->size = rd32(de + 28);
    }
    return true;
}

// Delete the entry at `off` plus its LFN run (fixed count from the name).
void dir_delete_at(uint64_t off, const char *name) {
    int slots = name_slots_total(name);
    for (int i = slots - 1; i >= 0; i--) {
        s_img[off - (uint64_t)i * 32] = 0xE5;
        flush_range(off - (uint64_t)i * 32, 1);
    }
    flush_range(off + 1, 31);
}

} // namespace

bool mounted() { return s_mounted; }
bool writable() { return s_writable; }
size_t count() { return s_count; }

bool stat(size_t i, const char **name_out, size_t *size_out) {
    if (i >= s_count) return false;
    if (name_out) *name_out = s_entries[i].path;
    if (size_out) *size_out = s_entries[i].size;
    return true;
}

bool read(const char *path, const void **data_out, size_t *size_out) {
    if (!path) return false;
    for (size_t i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir) continue;
        if (!path_eq(path, s_entries[i].path)) continue;
        if (data_out) *data_out = s_entries[i].data;
        if (size_out) *size_out = s_entries[i].size;
        return true;
    }
    return false;
}

bool is_dir(const char *path) {
    if (!path) return false;
    for (size_t i = 0; i < s_count; i++) {
        if (!s_entries[i].is_dir) continue;
        if (path_eq(path, s_entries[i].path)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public write API (ATA backend only).
// ---------------------------------------------------------------------------

bool create(const char *path) {
    return write_file(path, nullptr, 0);
}

bool write_file_at(const char *path, const void *data, size_t size,
                   uint8_t attr, bool create_dir);

bool write_file(const char *path, const void *data, size_t size) {
    return write_file_at(path, data, size, 0x20, false);
}

bool mkdir(const char *path) {
    return write_file_at(path, nullptr, 0, 0x10, true);
}

// Core create/replace. For directories the payload is the "." / ".." pair.
bool write_file_at(const char *path, const void *data, size_t size,
                   uint8_t attr, bool create_dir) {
    if (!s_mounted || !s_writable || !path) return false;
    char name[MAX_PATH];
    uint32_t parent = resolve_parent(path, name, sizeof(name));
    if (!parent || !name[0]) return false;
    if (name_slots_total(name) > 20) return false; // pathological

    // Existing entry: free its old chain in place keeps the slot.
    EntSnap snap{};
    uint64_t ent_off = 0;
    bool exists = lookup_full(path, &ent_off, &snap);
    if (exists && (snap.attr & 0x10) != 0 && !create_dir) return false; // EISDIR
    if (!exists && create_dir) {
        // fresh directory: allocate its content cluster up front
        snap.first = fat_alloc();
        snap.size = 0;
        if (!snap.first) return false;
        // (snap.attr unused on the create path)
    }

    uint32_t need = size ? (uint32_t)((size + (size_t)s_spc * s_bps - 1) / ((size_t)s_spc * s_bps)) : 0;
    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < need; i++) {
        uint32_t c = fat_alloc();
        if (!c) { if (first) fat_free_chain(first); if (!exists && create_dir) fat_free_chain(snap.first); return false; }
        if (!first) first = c;
        if (prev) fat_set(prev, c);
        prev = c;
    }

    if (exists && cluster_valid(snap.first) && !cluster_live(snap.first))
        fat_free_chain(snap.first);

    if (create_dir) {
        // "." and ".." live in the first (only) cluster of the new directory.
        uint64_t off = cluster_offset(snap.first);
        uint8_t *p = s_img + off;
        uint8_t dot[11] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        util::memcpy(p, dot, 11);
        p[11] = 0x10;
        wr16(p + 20, (uint16_t)(snap.first >> 16));
        wr16(p + 26, (uint16_t)(snap.first & 0xFFFF));
        wr32(p + 28, 0);
        uint8_t *q = p + 32;
        uint8_t dotdot[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        util::memcpy(q, dotdot, 11);
        q[11] = 0x10;
        uint32_t ppar = (parent == s_root_cluster) ? 0 : parent;
        wr16(q + 20, (uint16_t)(ppar >> 16));
        wr16(q + 26, (uint16_t)(ppar & 0xFFFF));
        wr32(q + 28, 0);
        flush_range(off, 64);
    }

    // Payload
    const uint8_t *src = (const uint8_t *)data;
    size_t left = size;
    uint32_t c = first;
    while (left && cluster_valid(c)) {
        uint64_t off = cluster_offset(c);
        size_t chunk = (size_t)s_spc * s_bps;
        if (chunk > left) chunk = left;
        if (!in_bounds(off, chunk)) break;
        if (src) util::memcpy(s_img + off, src, chunk);
        else util::memset(s_img + off, 0, chunk);
        flush_range(off, chunk);
        src += chunk;
        left -= chunk;
        c = fat_next(c);
    }

    // Directory entry
    uint8_t dir_attr = create_dir ? 0x10 : attr;
    if (!exists) {
        int slots = name_slots_total(name);
        uint64_t off = dir_find_free(parent, slots);
        if (!off) {
            if (first) fat_free_chain(first);
            if (create_dir) fat_free_chain(snap.first);
            return false;
        }
        dir_write_entry(off, name, dir_attr,
                        create_dir ? snap.first : first, (uint32_t)size, slots);
        flush_range(off, (uint64_t)slots * 32);
    } else {
        uint8_t *de = s_img + ent_off;
        uint32_t fc = create_dir ? snap.first : first;
        wr16(de + 20, (uint16_t)(fc >> 16));
        wr16(de + 26, (uint16_t)(fc & 0xFFFF));
        wr32(de + 28, (uint32_t)size);
        flush_range(ent_off, 32);
    }

    rescan();
    return true;
}

bool unlink(const char *path) {
    if (!s_mounted || !s_writable || !path) return false;
    EntSnap snap{};
    uint64_t off = 0;
    if (!lookup_full(path, &off, &snap)) return false;
    if (snap.attr & 0x10) {
        // Directories only when empty: "any real entry inside fails".
        uint32_t cl = snap.first;
        uint32_t guard = 0;
        while (cluster_valid(cl) && !cluster_eoc(cl) && ++guard < 4096) {
            uint64_t base = cluster_offset(cl);
            for (uint32_t s = 0; s < s_spc; s++) {
                uint64_t so = base + (uint64_t)s * s_bps;
                if (!in_bounds(so, s_bps)) break;
                const uint8_t *sec = s_img + so;
                for (uint32_t e = 0; e + 32 <= s_bps; e += 32) {
                    const uint8_t *de = sec + e;
                    if (!de[0]) goto dir_scanned;
                    if (de[0] == 0xE5) continue;
                    if (de[0] == '.') continue; // "." and ".."
                    if (de[11] == 0x0F) continue;
                    return false; // not empty
                }
            }
            cl = fat_next(cl);
        }
    dir_scanned:;
    }
    char name[MAX_PATH];
    if (!resolve_parent(path, name, sizeof(name))) return false;
    dir_delete_at(off, name);
    if (cluster_valid(snap.first) && !cluster_live(snap.first))
        fat_free_chain(snap.first);
    rescan();
    return true;
}

bool rename(const char *old_path, const char *new_path) {
    if (!s_mounted || !s_writable || !old_path || !new_path) return false;
    // Same parent only: keeps both the flat layout and the cluster chain.
    const char *oa = old_path, *na = new_path;
    while (*oa == '/') oa++;
    while (*na == '/') na++;
    const char *os = nullptr, *ns = nullptr;
    for (const char *p = oa; *p; p++) if (*p == '/') os = p;
    for (const char *p = na; *p; p++) if (*p == '/') ns = p;
    size_t op = os ? (size_t)(os - oa) : 0;
    size_t np = ns ? (size_t)(ns - na) : 0;
    if (op != np) return false;
    for (size_t i = 0; i < op; i++) if (oa[i] != na[i]) return false;

    EntSnap snap{};
    uint64_t off = 0;
    if (!lookup_full(old_path, &off, &snap)) return false;

    char old_name[MAX_PATH], new_name[MAX_PATH];
    uint32_t old_parent = resolve_parent(old_path, old_name, sizeof(old_name));
    uint32_t new_parent = resolve_parent(new_path, new_name, sizeof(new_name));
    if (!old_parent || old_parent != new_parent) return false;

    int old_slots = name_slots_total(old_name);
    int new_slots = name_slots_total(new_name);
    uint32_t parent = old_parent; (void)new_parent;

    if (old_slots == new_slots) {
        // In-place rewrite; the chain and size stay.
        dir_write_entry(off - (uint64_t)(old_slots - 1) * 32, new_name,
                        snap.attr, snap.first, snap.size, new_slots);
        flush_range(off - (uint64_t)(old_slots - 1) * 32, (uint64_t)new_slots * 32);
    } else {
        if (snap.attr & 0x10) return false; // directory slot-count changes: v1 no
        // Files: copy content, delete the old name, create the new one.
        const void *data = nullptr;
        size_t sz = 0;
        for (size_t i = 0; i < s_count; i++)
            if (!s_entries[i].is_dir && path_eq(s_entries[i].path, old_path)) {
                data = s_entries[i].data;
                sz = s_entries[i].size;
                break;
            }
        if (!data && sz) return false;
        unlink(old_path);
        if (!write_file(new_path, data, sz)) return false;
        (void)parent;
        return true;
    }
    rescan();
    return true;
}

// ---------------------------------------------------------------------------
// BPB parse + mount
// ---------------------------------------------------------------------------

bool mount_image(uint8_t *image, size_t size) {
    if (!image || size < 512) return false;
    if (image[510] != 0x55 || image[511] != 0xAA) return false;

    uint32_t bps = rd16(image + 11);
    if (bps != 512) {
        console::kprintf(ERR, BG, "[fat32] unsupported sector size %u\n", (unsigned)bps);
        return false;
    }
    uint32_t spc = image[13];
    uint32_t reserved = rd16(image + 14);
    uint32_t nfats = image[16];
    uint32_t fat_size = rd32(image + 36);
    if (!fat_size) fat_size = rd16(image + 22);
    // mtools (and some real volumes) put the count in the 16-bit field even
    // on FAT32; take whichever is non-zero.
    uint32_t total = rd32(image + 32);
    if (!total) total = rd16(image + 19);
    uint32_t root = rd32(image + 44);

    if (!spc || !nfats || !fat_size || !total) {
        console::println("[fat32] invalid BPB (zero geometry fields)", ERR);
        return false;
    }
    uint32_t data_start = reserved + nfats * fat_size;
    if (total <= data_start) {
        console::println("[fat32] invalid BPB (no data area)", ERR);
        return false;
    }
    uint64_t fat_bytes = (uint64_t)fat_size * bps;
    uint64_t fat_off = (uint64_t)reserved * bps;
    if (fat_off + fat_bytes > size) {
        console::println("[fat32] BPB geometry exceeds image size", ERR);
        return false;
    }
    if (root < 2) root = 2;

    s_bps = bps;
    s_spc = spc;
    s_fat_size = fat_size;
    s_reserved = reserved;
    s_nfats = nfats;
    s_data_start = data_start;
    s_cluster_count = (total - data_start) / spc;
    s_root_cluster = root;
    s_img = image;
    s_img_size = size;
    s_fat = image + fat_off;

    s_count = 0;
    flatten_dir(s_root_cluster, "disk/", 0);
    s_mounted = true;

    console::kprintf(OK, BG,
                     "[fat32] mounted: %u clusters, %u entries under /disk%s\n",
                     (unsigned)s_cluster_count, (unsigned)s_count,
                     s_writable ? " (writable volume)" : " (read-only image)");
    return true;
}

bool mount_ramdisk(const uint8_t *image, size_t size) {
    if (s_mounted) return true;
    s_writable = false;
    return mount_image((uint8_t *)image, size);
}

// Mount the persistent ATA volume on the secondary channel. The whole volume
// is read into RAM at mount; every mutation is flushed straight back, so the
// RAM copy is the cache, not a journal. Returns false when no disk answers —
// the caller then falls back to the boot module.
bool mount_ata() {
    if (s_mounted) return true;
    uint32_t sectors = 0;
    if (!ata::detect(&sectors)) return false;
    if (sectors < 2048) {
        console::kprintf(ERR, BG, "[fat32] volume too small: %u sectors\n", (unsigned)sectors);
        return false;
    }
    uint64_t bytes = (uint64_t)sectors * 512;
    uint8_t *img = (uint8_t *)heap::alloc((size_t)bytes);
    if (!img) {
        console::println("[fat32] no RAM for the volume cache", ERR);
        return false;
    }
    // Read the whole volume in 128-sector chunks (LBA28 count is one byte).
    // A PIO burst can intermittently ERR on emulated/long transfers, so retry
    // each chunk with progressively smaller granularity before giving up.
    uint32_t off_sec = 0;
    while (off_sec < sectors) {
        uint32_t chunk = sectors - off_sec;
        if (chunk > 128) chunk = 128;
        uint16_t *p = (uint16_t *)(img + (uint64_t)off_sec * 512);
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (attempt == 2) chunk = 16;
            if (attempt == 3) chunk = 1;
            ok = ata::rw(off_sec, (uint16_t)chunk, p, false);
        }
        if (!ok) {
            console::kprintf(ERR, BG, "[fat32] volume read failed at sector %u\n", (unsigned)off_sec);
            heap::free(img);
            return false;
        }
        off_sec += chunk;
    }
    s_writable = true;
    if (!mount_image(img, bytes)) {
        s_writable = false;
        heap::free(img);
        return false;
    }
    return true;
}

IoStats io_stats() {
    IoStats s;
    s.disk = ata::g_detected;
    s.sectors = ata::g_sectors;
    s.reads = ata::g_reads;
    s.writes = ata::g_writes;
    s.read_sectors = ata::g_read_sectors;
    s.write_sectors = ata::g_write_sectors;
    s.errors = ata::g_errors;
    return s;
}

uint64_t volume_bytes() { return s_mounted ? (uint64_t)s_img_size : 0; }

bool on_ata() { return s_mounted && s_writable; }

// Walks the in-memory FAT (s_fat), so this is a table scan, not disk I/O:
// ~32k reads for the 16 MiB sample volume, fine at a monitor's poll rate.
uint64_t free_bytes() {
    if (!s_mounted || !s_fat) return 0;
    uint64_t free_clusters = 0;
    for (uint32_t c = 2; c < s_cluster_count + 2; c++)
        if (fat_next(c) == 0) free_clusters++;
    return free_clusters * (uint64_t)s_spc * s_bps;
}

} // namespace choco::fs::fat32

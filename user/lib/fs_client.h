// user/lib/fs_client.h - filesystem helpers for ring-3 programs.
// Header-only. Talks only to the ChocoOS syscalls (fscount/fsstat/open/read/
// close); it does NOT know about FAT, initrd or procfs. Directory listings are
// derived from the flat path table: direct file children plus synthesized
// directories for deeper entries ("bin/ls" implies the "bin/" directory).
#pragma once
#include <stddef.h>
#include "../lib.h"

#define FS_MAX_ENT 128
#define FS_NAME_CAP 128

struct FsEnt {
    char name[FS_NAME_CAP]; // local name (no path); directories keep no slash
    unsigned long size;
    int is_dir;
};

// --- tiny string helpers (no libc in ring 3) ---
static inline size_t fs_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static inline int fs_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int fs_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static inline void fs_strcpy(char *d, const char *s, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static inline int fs_streq(const char *a, const char *b) { return fs_strcmp(a, b) == 0; }

// Normalize a directory path: always ends with '/' unless it is the root "".
static inline void fs_dir_norm(const char *in, char *out, size_t cap) {
    size_t l = fs_strlen(in), j = 0;
    for (size_t i = 0; i < l && j + 1 < cap; i++) out[j++] = in[i];
    if (j > 0 && out[j - 1] != '/' && j + 1 < cap) out[j++] = '/';
    out[j] = 0;
}

// Build a child path from a normalized dir and a name.
static inline void fs_join(const char *dir, const char *name, int is_dir, char *out,
                           size_t cap) {
    size_t n = 0;
    for (size_t i = 0; dir[i] && n + 1 < cap; i++) out[n++] = dir[i];
    for (size_t i = 0; name[i] && n + 1 < cap; i++) out[n++] = name[i];
    if (is_dir && n + 1 < cap) out[n++] = '/';
    out[n] = 0;
}

// Parent directory (normalized). Root's parent is root.
static inline void fs_parent(const char *dir, char *out, size_t cap) {
    size_t l = fs_strlen(dir);
    if (l && dir[l - 1] == '/') l--;         // drop trailing slash
    while (l > 0 && dir[l - 1] != '/') l--;  // to previous slash (kept)
    size_t n = 0;
    for (size_t i = 0; i < l && n + 1 < cap; i++) out[n++] = dir[i];
    out[n] = 0;
}

// List the direct children of `dir` (normalized, "" = root). Returns the count.
static inline int fs_list_dir(const char *dir, struct FsEnt *out, int max) {
    long n = choco_fscount();
    if (n < 0) return 0;
    size_t dl = fs_strlen(dir);
    int k = 0;
    for (long i = 0; i < n && k < max; i++) {
        char buf[FS_NAME_CAP];
        long sz = choco_fsstat(i, buf, FS_NAME_CAP);
        if (sz < 0) continue;
        if (fs_strncmp(buf, dir, dl) != 0) continue;
        const char *rest = buf + dl;
        if (!*rest) continue;
        size_t j = 0;
        while (rest[j] && rest[j] != '/') j++;
        if (rest[j] == 0) { // direct file child
            fs_strcpy(out[k].name, rest, FS_NAME_CAP);
            out[k].size = (unsigned long)sz;
            out[k].is_dir = 0;
            k++;
        } else { // a directory component (real FAT dir or synthesized)
            char comp[FS_NAME_CAP];
            size_t m = 0;
            for (size_t t = 0; t < j && m + 1 < FS_NAME_CAP; t++) comp[m++] = rest[t];
            comp[m] = 0;
            int dup = 0;
            for (int q = 0; q < k; q++)
                if (out[q].is_dir && fs_streq(out[q].name, comp)) { dup = 1; break; }
            if (dup) continue;
            fs_strcpy(out[k].name, comp, FS_NAME_CAP);
            out[k].size = 0;
            out[k].is_dir = 1;
            k++;
        }
    }
    return k;
}

// Member-wise copy: a plain struct assignment in a 136-byte type would make
// clang emit a memcpy call, and ring 3 has no libc.
static inline void fs_copy(struct FsEnt *d, const struct FsEnt *s) {
    fs_strcpy(d->name, s->name, FS_NAME_CAP);
    d->size = s->size;
    d->is_dir = s->is_dir;
}

// Directories first, then names alphabetically.
static inline void fs_sort(struct FsEnt *e, int n) {
    for (int i = 1; i < n; i++) {
        struct FsEnt key;
        fs_copy(&key, &e[i]);
        int j = i - 1;
        while (j >= 0) {
            int after = (e[j].is_dir < key.is_dir) ||
                        (e[j].is_dir == key.is_dir && fs_strcmp(e[j].name, key.name) > 0);
            if (!after) break;
            fs_copy(&e[j + 1], &e[j]);
            j--;
        }
        fs_copy(&e[j + 1], &key);
    }
}

// Read a whole file into buf; returns bytes read (or a negative errno).
static inline long fs_read_text(const char *path, char *buf, long cap) {
    long fd = choco_open(path);
    if (fd < 0) return fd;
    long total = 0;
    while (total < cap - 1) {
        long r = choco_read(fd, buf + total, (unsigned long)(cap - 1 - total));
        if (r <= 0) break;
        total += r;
    }
    buf[total] = 0;
    choco_close(fd);
    return total;
}

// Case-insensitive extension test (".txt"/".TXT"/".md"/".log"/".cfg").
static inline int fs_is_text_name(const char *name) {
    size_t l = fs_strlen(name);
    if (l < 4) return 0;
    const char *e = name + l - 4;
    if (e[0] != '.') return 0;
    char c0 = e[1], c1 = e[2], c2 = e[3];
    if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 + 32);
    if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
    if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
    return (c0 == 't' && c1 == 'x' && c2 == 't') || (c0 == 'c' && c1 == 'f' && c2 == 'g') ||
           (c0 == 'l' && c1 == 'o' && c2 == 'g') || (c0 == 'm' && c1 == 'd');
}

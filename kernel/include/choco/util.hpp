#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::util {

inline void *memset(void *d, int c, size_t n) {
    auto *p = (uint8_t *)d;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return d;
}
inline void *memcpy(void *d, const void *s, size_t n) {
    auto *a = (uint8_t *)d; auto *b = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}
inline int memcmp(const void *a, const void *b, size_t n) {
    auto *x = (const uint8_t *)a; auto *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) { if (x[i] != y[i]) return (int)x[i] - (int)y[i]; }
    return 0;
}
inline size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
inline bool parse_uint(const char *s, uint64_t &out) {
    if (!*s) return false;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    out = v;
    return true;
}
// Decimal rendering into buf (NUL-terminated). Returns digit count.
inline size_t to_str(unsigned long long v, char *out) {
    char t[32];
    int i = 31;
    t[i] = 0;
    if (v == 0) t[--i] = '0';
    while (v && i > 0) { t[--i] = (char)('0' + v % 10); v /= 10; }
    size_t n = 0;
    while (t[i]) { out[n++] = t[i++]; }
    out[n] = 0;
    return n;
}

inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }

} // namespace choco::util

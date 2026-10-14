// Freestanding runtime: the compiler emits calls to these for zero-init,
// struct assignment and bulk copies. Must stay dependency-free.
#include <stddef.h>

extern "C" {

void *memset(void *d, int c, size_t n) {
    auto *p = (unsigned char *)d;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    auto *a = (unsigned char *)d;
    auto *b = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    auto *a = (unsigned char *)d;
    auto *b = (const unsigned char *)s;
    if (a < b) {
        for (size_t i = 0; i < n; i++) a[i] = b[i];
    } else if (a > b) {
        for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1];
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    auto *x = (const unsigned char *)a;
    auto *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

} // extern "C"

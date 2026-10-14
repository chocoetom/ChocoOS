// ChocoOS user-space syscall library (ring 3, int $0x80).
// Numbers must match kernel/include/choco/user.hpp.
#pragma once

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_SLEEP   3
#define SYS_TICKS   4
#define SYS_YIELD   5
#define SYS_OPEN    6
#define SYS_CLOSE   7
#define SYS_SBRK    8
#define SYS_LSEEK   9
#define SYS_FSIZE   10
#define SYS_FSCOUNT 11
#define SYS_FSSTAT  12
#define SYS_WIN_CREATE  13
#define SYS_WIN_PRESENT 14
#define SYS_WIN_CLOSE   15
#define SYS_MKDIR       16
#define SYS_UNLINK      17
#define SYS_RENAME      18
#define SYS_RMDIR       19
#define SYS_IOCTL       20
#define SYS_WIN_RESIZE  21
#define SYS_FETCH       22

static long choco_sys(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static long choco_write(int fd, const char *buf, unsigned long len) {
    return choco_sys(SYS_WRITE, fd, (long)buf, (long)len);
}

static unsigned long choco_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void choco_print(const char *s) {
    choco_write(1, s, choco_strlen(s));
}

static void choco_print_num(long v) {
    char buf[24]; int i = 23;
    buf[i] = 0;
    unsigned long u = (unsigned long)((v < 0) ? -v : v);
    if (v < 0) buf[--i] = '-';
    if (u == 0) buf[--i] = '0';
    while (u > 0 && i > 0) { buf[--i] = '0' + (u % 10); u /= 10; }
    choco_print(&buf[i]);
}

static long choco_exit(long code) { return choco_sys(SYS_EXIT, code, 0, 0); }
static long choco_read(int fd, char *buf, unsigned long len) {
    return choco_sys(SYS_READ, fd, (long)buf, (long)len);
}
static long choco_sleep(unsigned long ms) { return choco_sys(SYS_SLEEP, (long)ms, 0, 0); }
static long choco_ticks(void) { return choco_sys(SYS_TICKS, 0, 0, 0); }
static long choco_yield(void) { return choco_sys(SYS_YIELD, 0, 0, 0); }
static long choco_open(const char *path) { return choco_sys(SYS_OPEN, (long)path, 0, 0); }
static long choco_close(long fd) { return choco_sys(SYS_CLOSE, fd, 0, 0); }
static long choco_sbrk(long incr) { return choco_sys(SYS_SBRK, incr, 0, 0); }
static long choco_lseek(long fd, long off, long whence) {
    return choco_sys(SYS_LSEEK, fd, off, whence);
}
static long choco_fsize(long fd) { return choco_sys(SYS_FSIZE, fd, 0, 0); }
static long choco_fscount(void) { return choco_sys(SYS_FSCOUNT, 0, 0, 0); }
static long choco_fsstat(long idx, char *buf, unsigned long cap) {
    return choco_sys(SYS_FSSTAT, idx, (long)buf, (long)cap);
}
static long choco_win_create(unsigned long w, unsigned long h, const char *title) {
    return choco_sys(SYS_WIN_CREATE, (long)w, (long)h, (long)title);
}
static long choco_win_present(long uaddr) {
    return choco_sys(SYS_WIN_PRESENT, uaddr, 0, 0);
}
static long choco_win_close(long uaddr) {
    return choco_sys(SYS_WIN_CLOSE, uaddr, 0, 0);
}
static long choco_open_flags(const char *path, long flags) {
    return choco_sys(SYS_OPEN, (long)path, flags, 0);
}
static long choco_mkdir(const char *path) { return choco_sys(SYS_MKDIR, (long)path, 0, 0); }
static long choco_unlink(const char *path) { return choco_sys(SYS_UNLINK, (long)path, 0, 0); }
static long choco_rmdir(const char *path) { return choco_sys(SYS_RMDIR, (long)path, 0, 0); }
static long choco_rename(const char *oldp, const char *newp) {
    return choco_sys(SYS_RENAME, (long)oldp, (long)newp, 0);
}
// Ask the WM for a new content size. It answers with UEV_RESIZE carrying the
// size actually applied (clamped to the buffer capacity), so treat that event
// as the source of truth rather than the value you requested.
static long choco_win_resize(long uaddr, long w, long h) {
    return choco_sys(SYS_WIN_RESIZE, uaddr, w, h);
}

// Kernel-side HTTP/1.0 fetch (DNS + TCP) into buf; returns bytes or <0. The
// ring-3 side cannot resolve hostnames itself, so the browser asks the kernel.
static long choco_fetch(const char *url, char *buf, unsigned long cap) {
    return choco_sys(SYS_FETCH, (long)url, (long)buf, (long)cap);
}

// Terminal ioctls (see kernel/include/choco/tty.hpp for the request numbers).
static long choco_ioctl(long fd, long request, void *arg) {
    return choco_sys(SYS_IOCTL, fd, request, (long)arg);
}

// Display-server shared window (must match kernel/gui/wm.cpp layout).
#define UW_PIX_OFF 4096
// Buffer capacity (>= the screen's content area), fixed at window creation, so
// a resize never invalidates the pixel pointer and the stride never changes.
// Must match UW_MAX_W/UW_MAX_H in kernel/gui/wm.cpp.
#define UW_MAX_W 1280
#define UW_MAX_H 800
#define UEV_MOUSE  1
#define UEV_KEY    2
#define UEV_RESIZE 3
struct uw_ev {
    long type, x, y, info;
};
struct uw_win {
    long magic, w, h, dirty, head, tail;
    struct uw_ev ev[32];
    long pitch;   // row stride in pixels; always UW_MAX_W (the capacity) so a
                  // resize never invalidates the pixel pointer.
};

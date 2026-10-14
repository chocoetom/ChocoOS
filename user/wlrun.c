// /bin/wlrun - minimal Wayland server + a demo client in one binary.
//
// The parent process is a tiny Wayland compositor: it owns a ChocoOS desktop
// window (win_create syscall, exposed to the Linux ABI as 500) and speaks just
// enough of the Wayland protocol - wl_display, wl_registry, wl_compositor,
// wl_shm, wl_shm_pool, wl_surface, wl_buffer - over an AF_UNIX socket. A forked
// child is a raw-marshalling client (no libwayland) that binds those globals,
// attaches an shm buffer and commits it. The compositor blits the buffer into
// the window, proving the whole path: socket + fd passing (SCM_RIGHTS) + shared
// memory + desktop window.
//
// This is the seed of the window protocol for ported Linux apps (ROADMAP 25).
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SYS_WIN_CREATE
#define SYS_WIN_CREATE 500
#define SYS_WIN_PRESENT 501
#define SYS_WIN_CLOSE 502
#endif
#define UW_PIX_OFF 4096

// Must match struct uw_win in user/lib.h.
struct uw_ev { int64_t type, x, y, info; };
struct uw_win {
    int64_t magic, w, h, dirty, head, tail;
    struct uw_ev ev[32];
    int64_t pitch;
};

#define WIN_W 640
#define WIN_H 480
#define SOCK_PATH "/wayland-0"

// --- Wayland wire helpers ---------------------------------------------------
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// Request/event header: [object id][ (size<<16) | opcode ].
static void wl_send(int fd, uint32_t id, uint16_t op, const void *args, uint32_t n) {
    uint8_t buf[512];
    put32(buf, id);
    put32(buf + 4, ((8 + n) << 16) | op);
    if (n) memcpy(buf + 8, args, n);
    ssize_t w = write(fd, buf, 8 + n);
    (void)w;
}

// --- compositor state -------------------------------------------------------
enum { O_FREE = 0, O_DISPLAY, O_REGISTRY, O_COMPOSITOR, O_SURFACE, O_SHM, O_POOL, O_BUFFER, O_CALLBACK };
struct Obj { uint32_t id; uint8_t type; };
static struct Obj g_obj[64];

static int g_winfd_ok;
static int g_done; // set once a frame is composited; the demo exits after one
static unsigned *g_winpx;
static int g_pitch = WIN_W;
static int g_win_addr = -1;

// Attached surface state (one surface for the demo).
static uint8_t *g_pool_map;      // mmap of the client's wl_shm pool
static size_t g_pool_len;
static int g_buf_w, g_buf_h, g_buf_stride;
static uint8_t *g_buf_ptr;       // pool_map + offset
static int g_buf_attached;
static uint32_t g_frame_cb;      // pending wl_callback id (0 = none)

static struct Obj *obj_find(uint32_t id) {
    for (int i = 0; i < 64; i++)
        if (g_obj[i].id == id && g_obj[i].type != O_FREE) return &g_obj[i];
    return NULL;
}
static struct Obj *obj_add(uint32_t id, uint8_t type) {
    for (int i = 0; i < 64; i++)
        if (g_obj[i].type == O_FREE) { g_obj[i].id = id; g_obj[i].type = type; return &g_obj[i]; }
    return NULL;
}
static void obj_del(uint32_t id) {
    struct Obj *o = obj_find(id);
    if (o) o->type = O_FREE;
}

static void wl_global(int cfd, uint32_t name, const char *iface, uint32_t ver) {
    uint8_t a[256];
    uint32_t n = strlen(iface) + 1;
    uint32_t sz = 4 + 4 + n + ((4 - (n & 3)) & 3) + 4;
    put32(a, name);
    put32(a + 4, n);
    memcpy(a + 8, iface, n);
    put32(a + 8 + n + ((4 - (n & 3)) & 3), ver);
    wl_send(cfd, 1 /*registry id*/, 0 /*global*/, a, sz);
}

static void present(void) {
    if (g_buf_attached && g_winfd_ok && g_buf_ptr) {
        int rows = g_buf_h < WIN_H ? g_buf_h : WIN_H;
        int cols = g_buf_w < WIN_W ? g_buf_w : WIN_W;
        for (int y = 0; y < rows; y++) {
            unsigned *dst = g_winpx + (size_t)y * g_pitch;
            const unsigned *src = (const unsigned *)(g_buf_ptr + (size_t)y * g_buf_stride);
            for (int x = 0; x < cols; x++) dst[x] = src[x];
        }
        syscall(SYS_WIN_PRESENT, g_win_addr, 0, 0);
        static int once = 0;
        if (!once) { once = 1; g_done = 1; fprintf(stderr, "wlrun: composite OK\n"); }
    }
}

// Handle one client request message.
static void handle_request(int cfd, uint32_t id, uint16_t op, const uint8_t *args) {
    struct Obj *o = obj_find(id);
    if (!o) return;
    switch (o->type) {
    case O_DISPLAY:
        if (op == 1) { // get_registry(new_id)
            uint32_t nid; memcpy(&nid, args, 4);
            obj_add(nid, O_REGISTRY);
            wl_global(cfd, 1, "wl_compositor", 4);
            wl_global(cfd, 2, "wl_shm", 1);
        } else if (op == 0) { // sync(new_id) -> callback
            uint32_t nid; memcpy(&nid, args, 4);
            obj_add(nid, O_CALLBACK);
            uint32_t data = 1;
            wl_send(cfd, nid, 0, &data, 4);
            obj_del(nid);
        }
        break;
    case O_REGISTRY: {
        if (op == 0) { // bind(name, interface:string, version:u32, new_id)
            uint32_t name; memcpy(&name, args, 4);
            uint32_t len; memcpy(&len, args + 4, 4);
            const char *iface = (const char *)(args + 8);
            uint32_t off = 8 + len + ((4 - (len & 3)) & 3);
            uint32_t nid; memcpy(&nid, args + off + 4, 4);
            uint8_t t = 0;
            if (!strcmp(iface, "wl_compositor")) t = O_COMPOSITOR;
            else if (!strcmp(iface, "wl_shm")) t = O_SHM;
            if (t) obj_add(nid, t);
        }
        break;
    }
    case O_COMPOSITOR:
        if (op == 0) { uint32_t nid; memcpy(&nid, args, 4); obj_add(nid, O_SURFACE); }
        break;
    case O_SHM:
        if (op == 0) { // create_pool(new_id, fd) - fd arrives via SCM_RIGHTS
            uint32_t nid; memcpy(&nid, args, 4);
            obj_add(nid, O_POOL);
        }
        break;
    case O_POOL:
        if (op == 0) { // create_buffer(new_id, offset, w, h, stride, format)
            uint32_t nid, off, w, h, stride, fmt;
            memcpy(&nid, args, 4); memcpy(&off, args + 4, 4);
            memcpy(&w, args + 8, 4); memcpy(&h, args + 12, 4);
            memcpy(&stride, args + 16, 4); memcpy(&fmt, args + 20, 4);
            (void)fmt;
            obj_add(nid, O_BUFFER);
            g_buf_w = (int)w; g_buf_h = (int)h; g_buf_stride = (int)stride;
            g_buf_ptr = g_pool_map ? g_pool_map + off : NULL;
        }
        break;
    case O_SURFACE:
        if (op == 1) { // attach(buffer, x, y)
            uint32_t bufid; memcpy(&bufid, args, 4);
            g_buf_attached = (bufid != 0);
        } else if (op == 3) { // frame(new_id)
            uint32_t nid; memcpy(&nid, args, 4);
            obj_add(nid, O_CALLBACK);
            g_frame_cb = nid;
        } else if (op == 6) { // commit
            present();
            g_buf_attached = 0;
            if (g_frame_cb) {
                uint32_t data = 1;
                wl_send(cfd, g_frame_cb, 0, &data, 4); // wl_callback.done
                obj_del(g_frame_cb);
                g_frame_cb = 0;
            }
        }
        break;
    default: break;
    }
}

// --- the forked demo client -------------------------------------------------
static void client_run(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { fprintf(stderr, "[wlclient] socket fail\n"); _exit(3); }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, SOCK_PATH);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "[wlclient] connect fail errno=%d\n", errno);
        _exit(4);
    }

    // Create the shm pool: memfd + ftruncate + mmap + draw a pattern.
    int fd = (int)syscall(319, "buf", 0);
    if (fd < 3) _exit(5);
    size_t pool_len = WIN_W * WIN_H * 4;
    if (ftruncate(fd, pool_len) != 0) _exit(6);
    unsigned *px = mmap(NULL, pool_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) _exit(7);
    for (int y = 0; y < WIN_H; y++)
        for (int x = 0; x < WIN_W; x++) {
            unsigned r = (unsigned)x * 255 / (WIN_W - 1);
            unsigned g = (unsigned)y * 255 / (WIN_H - 1);
            px[y * WIN_W + x] = (r << 16) | (g << 8) | 0x40; // 0x00RRGGBB
        }

    // wl_display.get_registry(new_id=2)
    uint8_t a[64];
    put32(a, 2);
    wl_send(s, 1 /*display*/, 1, a, 4);

    // wl_registry.bind(1, "wl_compositor", 4, new_id=3)
    uint32_t off = 0;
    put32(a + off, 1); off += 4;               // name
    const char *ci = "wl_compositor"; uint32_t cl = strlen(ci) + 1;
    put32(a + off, cl); off += 4; memcpy(a + off, ci, cl); off += cl;
    off += (4 - (off & 3)) & 3;
    put32(a + off, 4); off += 4;               // version
    put32(a + off, 3); off += 4;               // new_id
    wl_send(s, 2 /*registry*/, 0, a, off);

    // wl_registry.bind(2, "wl_shm", 1, new_id=4)
    off = 0;
    put32(a + off, 2); off += 4;
    const char *si = "wl_shm"; uint32_t sl = strlen(si) + 1;
    put32(a + off, sl); off += 4; memcpy(a + off, si, sl); off += sl;
    off += (4 - (off & 3)) & 3;
    put32(a + off, 1); off += 4;
    put32(a + off, 4); off += 4;
    wl_send(s, 2, 0, a, off);

    // wl_compositor.create_surface(new_id=5)
    put32(a, 5);
    wl_send(s, 3, 0, a, 4);

    // wl_shm.create_pool(new_id=6, fd) - the fd rides on SCM_RIGHTS. Build the
    // full message (header + arg) so it goes in one sendmsg with the fd.
    uint8_t pool_msg[12];
    put32(pool_msg, 4);              // target: the wl_shm object
    put32(pool_msg + 4, (12 << 16) | 0); // size 12, opcode create_pool
    put32(pool_msg + 8, 6);          // new_id
    struct iovec iov = { pool_msg, 12 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    if (sendmsg(s, &m, 0) < 0) _exit(8);

    // wl_shm_pool.create_buffer(new_id=7, offset=0, w, h, stride, format=0)
    off = 0;
    put32(a + off, 7); off += 4;
    put32(a + off, 0); off += 4;
    put32(a + off, WIN_W); off += 4;
    put32(a + off, WIN_H); off += 4;
    put32(a + off, WIN_W * 4); off += 4;
    put32(a + off, 0 /*ARGB8888*/); off += 4;
    wl_send(s, 6, 0, a, off);

    // wl_surface.attach(buffer=7, 0, 0) then commit.
    off = 0;
    put32(a + off, 7); off += 4;
    put32(a + off, 0); off += 4;
    put32(a + off, 0); off += 4;
    wl_send(s, 5, 1, a, off);
    wl_send(s, 5, 6, a, 0); // commit

    // Read a bit of the reply stream so the compositor's writes flush, then exit.
    char rb[256];
    struct pollfd p = { s, POLLIN, 0 };
    for (int i = 0; i < 20; i++) {
        if (poll(&p, 1, 100) > 0) { ssize_t r = read(s, rb, sizeof rb); if (r <= 0) break; }
    }
    _exit(0);
}

int main(void) {
    // Own a desktop window.
    long addr = syscall(SYS_WIN_CREATE, WIN_W, WIN_H, "Wayland");
    if (addr < 0) { fprintf(stderr, "wlrun: win_create failed (%ld)\n", addr); return 1; }
    g_win_addr = (int)addr;
    struct uw_win *w = (struct uw_win *)addr;
    g_pitch = (int)w->pitch;
    if (g_pitch < WIN_W) g_pitch = WIN_W;
    g_winpx = (unsigned *)(addr + UW_PIX_OFF);
    g_winfd_ok = 1;

    // Listener.
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, SOCK_PATH);
    unlink(SOCK_PATH);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0) { fprintf(stderr, "wlrun: bind failed\n"); return 1; }
    if (listen(ls, 4) != 0) { fprintf(stderr, "wlrun: listen failed\n"); return 1; }

    pid_t pid = fork();
    if (pid == 0) { client_run(); _exit(0); }

    // Accept the client, then run the request loop.
    int cfd = -1;
    for (int i = 0; i < 200 && cfd < 0; i++) {
        cfd = accept(ls, NULL, NULL);
        if (cfd < 0) usleep(10000);
    }
    if (cfd < 0) { fprintf(stderr, "wlrun: no client\n"); return 1; }

    obj_add(1, O_DISPLAY); // object 1 is always wl_display

    uint8_t rbuf[8192];
    size_t have = 0;
    for (;;) {
        struct pollfd p = { cfd, POLLIN, 0 };
        int pr = poll(&p, 1, 1000);
        if (pr < 0) break;
        if (pr == 0) continue;
        // Receive with an ancillary buffer for SCM_RIGHTS fds.
        char cbuf[CMSG_SPACE(4 * sizeof(int))];
        struct iovec iov = { rbuf + have, sizeof rbuf - have };
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = &iov; m.msg_iovlen = 1;
        m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
        ssize_t r = recvmsg(cfd, &m, 0);
        if (r <= 0) break;
        have += (size_t)r;
        // Install any received fd by mmap-ing it as the shm pool.
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int nfd;
                memcpy(&nfd, CMSG_DATA(c), sizeof nfd);
                g_pool_len = WIN_W * WIN_H * 4;
                g_pool_map = mmap(NULL, g_pool_len, PROT_READ, MAP_SHARED, nfd, 0);
                if (g_pool_map == MAP_FAILED) g_pool_map = NULL;
            }
        }
        // Parse complete messages: [id][size<<16|op][args...]
        size_t off = 0;
        while (have - off >= 8) {
            uint32_t id, w2;
            memcpy(&id, rbuf + off, 4);
            memcpy(&w2, rbuf + off + 4, 4);
            uint16_t op = (uint16_t)(w2 & 0xFFFF);
            uint16_t sz = (uint16_t)(w2 >> 16);
            if (sz < 8 || have - off < sz) break;
            handle_request(cfd, id, op, rbuf + off + 8);
            off += sz;
        }
        if (off) { memmove(rbuf, rbuf + off, have - off); have -= off; }
        if (g_done) break; // one composited frame is enough for the demo
    }
    if (g_win_addr >= 0) syscall(SYS_WIN_CLOSE, g_win_addr, 0, 0);
    return 0;
}

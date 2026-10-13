// Ring-3 syscall handler (int $0x80). All pointers from user mode are
// validated: low-half canonical + every page mapped in the CURRENT space.
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/gui.hpp>
#include <choco/mem.hpp>
#include <choco/net.hpp>
#include <choco/rtc.hpp>
#include <choco/sched.hpp>
#include <choco/tty.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

using choco::console::BG;
using choco::console::FG;

namespace choco::syscall {
namespace {

// User addresses must live in the low half and be fully mapped.
bool user_range_ok(uint64_t ptr, uint64_t len) {
    if (len == 0) return true;
    if (ptr >= 0x0000800000000000ULL) return false;
    if (ptr + len < ptr) return false; // wrap
    if (ptr + len > 0x0000800000000000ULL) return false;
    for (uint64_t p = ptr & ~0xFFFULL; p < ptr + len; p += 4096)
        if (paging::translate(p) == 0) return false;
    return true;
}

bool user_cstr_ok(uint64_t ptr, size_t cap = 256) {
    if (ptr >= 0x0000800000000000ULL) return false;
    for (size_t i = 0; i < cap; i++) {
        if (!user_range_ok(ptr + i, 1)) return false;
        if (((const char *)ptr)[i] == 0) return true;
    }
    return false; // unterminated within cap
}

// Open-file table for the foreground process (single task at a time, so a
// small global table is enough; cleared on each run).
struct Open {
    bool used = false;
    bool pipe = false;
    unsigned pipe_id = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t off = 0;
    int writable = -1;
    bool append = false;
    bool is_sock = false; // internet socket fd
    int sock = -1;        // net::tcp handle once connected
    bool upair = false;   // AF_UNIX socketpair end
    unsigned tx = 0, rx = 0; // pipe ids for the two directions
    uint32_t b_ip = 0;    // bind() address (0 = any)
    uint16_t b_port = 0;  // bind() port
    bool is_unix = false; // AF_UNIX (path) socket
    int ulist = -1;       // named Unix listener index once bound (-1 = not bound)
};
constexpr unsigned FD_BASE = 3, FD_N = 8;
struct FdSet { Open open[FD_N]{}; Open std[3]{}; };
constexpr unsigned FD_CTX_N = 256;
FdSet s_fds[FD_CTX_N]{};
FdSet *fds_current() { return &s_fds[sched::current_pid() & (FD_CTX_N - 1)]; }
struct Pipe { char data[4096]{}; unsigned head = 0, tail = 0, count = 0; bool used = false; };
constexpr unsigned PIPE_N = 8;
Pipe s_pipe[PIPE_N]{};
uint32_t s_tty_pgid = 1;
int alloc_pipe();
void free_pipe_if_unused(unsigned id);
void unix_unbind(int li);
int unix_find(const char *path, bool need_listen);
int unix_alloc();
bool unix_has_pending(int li);
long unix_read_path(uint64_t addr, char *out, size_t cap);

// A named AF_UNIX listener. bind() reserves a slot and records the path;
// listen() marks it joinable; connect() pushes a server-side endpoint into the
// queue, which accept() pops into a fresh fd. Endpoints are two one-way pipes,
// the same mechanism socketpair uses.
struct UnixSock {
    bool used = false;
    bool listening = false;
    char path[72]{};
    struct Slot { bool used = false; unsigned tx = 0, rx = 0; } q[4];
};
constexpr unsigned UNIX_N = 8;
UnixSock s_unix[UNIX_N]{};

void fds_reset() {
    for (auto &set : s_fds) set = FdSet{};
    for (auto &p : s_pipe) util::memset(&p, 0, sizeof(Pipe)); // no 4 KiB temp on the syscall stack
    for (auto &u : s_unix) u = UnixSock{};
}

Open *fd_get(uint64_t fd) {
    FdSet *set = fds_current();
    if (fd < 3) return set->std[fd].used ? &set->std[fd] : nullptr;
    if (fd < FD_BASE || fd >= FD_BASE + FD_N) return nullptr;
    Open &o = set->open[fd - FD_BASE];
    return o.used ? &o : nullptr;
}

unsigned pipe_refs(unsigned id, bool readers) {
    unsigned refs = 0;
    for (unsigned c = 0; c < FD_CTX_N; c++) {
        const FdSet &set = s_fds[c];
        for (unsigned i = 0; i < FD_N; i++) {
            const Open &o = set.open[i];
            if (o.used && o.pipe && o.pipe_id == id && ((i & 1) == 0) == readers) refs++;
        }
        for (unsigned i = 0; i < 3; i++) {
            const Open &o = set.std[i];
            if (o.used && o.pipe && o.pipe_id == id && ((i == 0) == readers)) refs++;
        }
    }
    return refs;
}

long do_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (len > 65536) len = 65536;
    if (!user_range_ok(buf, len)) return -14; // EFAULT
    if ((fd == 1 || fd == 2) && !fd_get(fd)) {
        // Route console output through the tty so OPOST and IXON apply.
        return tty::write_out((const char *)buf, (size_t)len);
    }
    Open *o = fd_get(fd);
    if (!o) return -9; // EBADF
    if (o->is_sock) {
        if (o->sock < 0) return -107; // ENOTCONN
        if (len > 65535) len = 65535;
        int n = net::tcp::send(o->sock, (const void *)buf, (uint16_t)len);
        return n < 0 ? -32 : n; // EPIPE
    }
    if (o->upair) { // AF_UNIX socketpair: push into the tx pipe
        Pipe &p = s_pipe[o->tx];
        const char *s = (const char *)buf;
        uint64_t n = 0;
        while (n < len && p.count < sizeof(p.data)) {
            p.data[p.tail] = s[n++];
            p.tail = (p.tail + 1) % (unsigned)sizeof(p.data);
            p.count++;
        }
        return (long)n;
    }
    if (o->writable >= 0) {
        size_t next = o->off;
        long n = fs::writable_write(o->writable, o->off, (const void *)buf,
                                    (size_t)len, o->append, &next);
        if (n >= 0) {
            o->off = next;
            o->size = next > o->size ? next : o->size;
            fs::disk_persistence_flush();
        }
        return n;
    }
    if (!o->pipe) return -9;
    Pipe &p = s_pipe[o->pipe_id];
    if (!pipe_refs(o->pipe_id, true)) return -32; // EPIPE / SIGPIPE default
    const char *s = (const char *)buf;
    uint64_t n = 0;
    while (n < len && p.count < sizeof(p.data)) {
        p.data[p.tail] = s[n++];
        p.tail = (p.tail + 1) % sizeof(p.data);
        p.count++;
    }
    return (long)n;
}

long do_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (len == 0) return 0;
    if (len > 4096) len = 4096;
    if (!user_range_ok(buf, len)) return -14;
    char *d = (char *)buf;
    if (fd == 0 && !fd_get(fd)) {
        // Cooked by the line discipline: a canonical read returns a finished
        // line; a raw read returns as soon as a byte is available. A handled
        // signal (SIGWINCH, SIGINT) unblocks the read with EINTR.
        size_t i = 0;
        for (;;) {
            char c;
            if (tty::get_cooked(c)) {
                d[i++] = c;
                if (c == '\n' || i == len) break;
                continue;
            }
            if (i) break;
            if (tty::take_eof()) return 0;
            if (proc::has_pending()) return -4; // EINTR
            sched::yield(); // blocking read: let others run
        }
        return (long)i;
    }
    Open *o = fd_get(fd);
    if (!o) return -9;
    if (o->is_sock) {
        if (o->sock < 0) return -107; // ENOTCONN
        choco::cpu::sti(); // blocking recv needs the PIT/NIC IRQ (see do_connect)
        int n = net::tcp::recv(o->sock, d, (uint16_t)len, 30000);
        return n < 0 ? -11 : n; // EAGAIN
    }
    if (o->upair) { // AF_UNIX socketpair: pop from the rx pipe
        Pipe &p = s_pipe[o->rx];
        size_t n = p.count < len ? p.count : (size_t)len;
        for (size_t i = 0; i < n; i++) {
            d[i] = p.data[p.head];
            p.head = (p.head + 1) % (unsigned)sizeof(p.data);
            p.count--;
        }
        return (long)n;
    }
    if (o->pipe) {
        // Block until data arrives; return EOF (0) only once every write end is
        // closed, so a reader like `busybox wc` does not stop on an empty pipe.
        for (;;) {
            Pipe &p = s_pipe[o->pipe_id];
            if (p.count) {
                size_t n = p.count < len ? p.count : (size_t)len;
                for (size_t i = 0; i < n; i++) {
                    d[i] = p.data[p.head];
                    p.head = (p.head + 1) % sizeof(p.data);
                    p.count--;
                }
                return (long)n;
            }
            if (!pipe_refs(o->pipe_id, false)) return 0; // no writers left: EOF
            if (proc::has_pending()) return -4;          // EINTR
            sched::yield();
        }
    }
    size_t n = o->size > o->off ? o->size - o->off : 0;
    if (n > len) n = (size_t)len;
    for (size_t i = 0; i < n; i++) d[i] = (char)o->data[o->off + i];
    o->off += n;
    return (long)n;
}

// Linux x86-64 terminal ioctls. The console is a small, always-present
// controlling tty for descriptors 0, 1 and 2; the line discipline in
// choco::tty owns the termios/winsize state so musl and BusyBox observe a
// coherent structure and TCSETS actually switches raw/canonical mode.
long do_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    if (fd > 2) return -25; // only the three console descriptors are ttys

    // Integer-argument ioctls: no user pointer to validate.
    if (request == tty::TCXONC) return 0; // start/stop output: accepted
    if (request == tty::TCFLSH) {         // queue selector: we only have input
        tty::flush_input();
        return 0;
    }

    size_t arg_size =
        (request == tty::TCGETS || request == tty::TCSETS || request == tty::TCSETSW ||
         request == tty::TCSETSF) ? sizeof(tty::Termios)
        : (request == tty::TIOCSTI) ? 1
        : (request == tty::TIOCGWINSZ || request == tty::TIOCSWINSZ) ? 8
                                                                    : 4;
    if (!user_range_ok(arg, arg_size)) return -14; // EFAULT

    if (request == tty::TCGETS) {
        tty::get_termios(*(tty::Termios *)arg);
        return 0;
    }
    if (request == tty::TCSETS || request == tty::TCSETSW || request == tty::TCSETSF) {
        tty::set_termios(*(const tty::Termios *)arg);
        if (request == tty::TCSETSF) tty::flush_input();
        return 0;
    }
    if (request == tty::TIOCGWINSZ) {
        uint16_t *w = (uint16_t *)arg;
        tty::get_winsize(w[0], w[1]);
        w[2] = 0;
        w[3] = 0;
        return 0;
    }
    if (request == tty::TIOCSWINSZ) {
        const uint16_t *w = (const uint16_t *)arg;
        tty::set_winsize(w[0], w[1]);
        return 0;
    }
    if (request == tty::FIONREAD) {
        *(uint32_t *)arg = tty::cooked_count();
        return 0;
    }
    if (request == tty::TIOCGPGRP) {
        *(uint32_t *)arg = tty::fg_pgid();
        return 0;
    }
    if (request == tty::TIOCSPGRP) {
        uint32_t pgid = *(const uint32_t *)arg;
        if (!pgid) return -22;
        s_tty_pgid = pgid;
        tty::set_fg_pgid(pgid);
        return 0;
    }
    if (request == tty::TIOCSTI) {
        tty::inject(*(const char *)arg);
        return 0;
    }
    return -25; // ENOTTY
}

long do_open(uint64_t path, uint64_t flags) {
    if (!user_cstr_ok(path)) return -14;
    const void *data = nullptr;
    size_t size = 0;
    int writable = -1;
    bool want_write = (flags & 3) != 0;
    bool want_create = (flags & 0x40) != 0;
    bool want_trunc = (flags & 0x200) != 0;
    if (want_write || want_create || want_trunc) {
        long wi = fs::writable_open((const char *)path, want_create, want_trunc,
                                    (const uint8_t **)&data, &size);
        if (wi >= 0) writable = (int)wi;
        else if (wi != -2) return wi;
        else {
            const void *existing = nullptr; size_t existing_size = 0;
            bool exists = fs::read((const char *)path, &existing, &existing_size);
            if (want_write || want_trunc) return -30; // EROFS for initrd
            if (!exists) return -2;
        }
    }
    if (!fs::read((const char *)path, &data, &size)) return -2; // ENOENT
    FdSet *set = fds_current();
    for (unsigned i = 0; i < FD_N; i++) {
        if (set->open[i].used) continue;
        set->open[i].used = true;
        set->open[i].data = (const uint8_t *)data;
        set->open[i].size = size;
        set->open[i].off = 0;
        set->open[i].writable = writable;
        set->open[i].append = (flags & 0x400) != 0;
        fs::disk_persistence_flush();
        return (long)(FD_BASE + i);
    }
    return -24; // EMFILE
}

long do_close(uint64_t fd) {
    FdSet *set = fds_current();
    if (fd < 3) {
        if (!set->std[fd].used) return -9;
        set->std[fd] = Open{};
        return 0;
    }
    Open *o = fd_get(fd);
    if (!o) return -9;
    if (o->is_sock && o->sock >= 0) net::tcp::close(o->sock);
    if (o->pipe) free_pipe_if_unused(o->pipe_id);
    if (o->upair) { free_pipe_if_unused(o->tx); free_pipe_if_unused(o->rx); }
    if (o->is_unix && o->ulist >= 0) unix_unbind(o->ulist);
    *o = Open{};
    return 0;
}

// --- AF_UNIX named sockets --------------------------------------------------
// Release a named endpoint: drop any still-queued server halves and free the
// slot so the path can be rebound. Already-accepted endpoints keep their own
// pipes (they are referenced by a descriptor, so free_pipe_if_unused skips them).
void unix_unbind(int li) {
    if (li < 0 || (unsigned)li >= UNIX_N) return;
    UnixSock &u = s_unix[li];
    for (unsigned k = 0; k < 4; k++) {
        if (!u.q[k].used) continue;
        free_pipe_if_unused(u.q[k].tx);
        free_pipe_if_unused(u.q[k].rx);
        u.q[k].used = false;
    }
    u = UnixSock{};
}

int unix_find(const char *path, bool need_listen) {
    for (unsigned i = 0; i < UNIX_N; i++) {
        if (!s_unix[i].used) continue;
        if (need_listen && !s_unix[i].listening) continue;
        const char *a = s_unix[i].path;
        size_t j = 0;
        while (a[j] && path[j] && a[j] == path[j]) j++;
        if (a[j] == path[j]) return (int)i; // both hit NUL together
    }
    return -1;
}

int unix_alloc() {
    for (unsigned i = 0; i < UNIX_N; i++)
        if (!s_unix[i].used) return (int)i;
    return -1;
}

bool unix_has_pending(int li) {
    if (li < 0 || (unsigned)li >= UNIX_N || !s_unix[li].listening) return false;
    for (unsigned k = 0; k < 4; k++)
        if (s_unix[li].q[k].used) return true;
    return false;
}

// Read the sun_path embedded in a user sockaddr_un (family at [0..1], path from
// [2]). Returns the path length (>=1) or a negative errno.
long unix_read_path(uint64_t addr, char *out, size_t cap) {
    if (!user_range_ok(addr, 2)) return -14;
    if (((const uint8_t *)addr)[0] != 1) return -97; // AF_UNIX
    for (size_t i = 0; i + 1 < cap; i++) {
        if (!user_range_ok(addr + 2 + i, 1)) return -14;
        char c = (char)((const uint8_t *)addr)[2 + i];
        out[i] = c;
        if (!c) return (long)(i + 1);
    }
    out[cap - 1] = 0;
    return (long)cap;
}

// A pipe id is free when no open descriptor references it.
void free_pipe_if_unused(unsigned id) {
    for (auto &set : s_fds)
        for (unsigned i = 0; i < FD_N; i++) {
            const Open &o = set.open[i];
            if (!o.used) continue;
            if ((o.pipe && o.pipe_id == id) || (o.upair && (o.tx == id || o.rx == id))) return;
        }
    s_pipe[id] = Pipe{};
}
int alloc_pipe() {
    for (unsigned k = 0; k < PIPE_N; k++)
        if (!s_pipe[k].used) { s_pipe[k] = Pipe{}; s_pipe[k].used = true; return (int)k; }
    return -1;
}

long do_pipe(uint64_t out, uint64_t flags) {
    if (!user_range_ok(out, 16)) return -14;
    FdSet *set = fds_current();
    for (unsigned i = 0; i + 1 < FD_N; i += 2) {
        if (set->open[i].used || set->open[i + 1].used) continue;
        int id = alloc_pipe();
        if (id < 0) return -24;
        set->open[i] = Open{true, true, (unsigned)id, nullptr, 0, 0};
        set->open[i + 1] = Open{true, true, (unsigned)id, nullptr, 0, 0};
        *(uint32_t *)out = FD_BASE + i;
        *(uint32_t *)(out + 4) = FD_BASE + i + 1;
        (void)flags;
        return 0;
    }
    return -24;
}

// socketpair(AF_UNIX, SOCK_STREAM): two connected endpoints, each a pair of
// one-way pipes (a -> b and b -> a).
long do_socketpair(uint64_t domain, uint64_t type, uint64_t proto, uint64_t sv) {
    (void)proto;
    if (domain != 1) return -97; // AF_UNIX
    if (type != 1) return -94;   // SOCK_STREAM
    if (!user_range_ok(sv, 8)) return -14;
    FdSet *set = fds_current();
    int i0 = -1, i1 = -1;
    for (unsigned i = 0; i < FD_N && i1 < 0; i++)
        if (!set->open[i].used) { if (i0 < 0) i0 = (int)i; else i1 = (int)i; }
    if (i0 < 0 || i1 < 0) return -24; // EMFILE
    int pa = alloc_pipe(), pb = alloc_pipe();
    if (pa < 0 || pb < 0) {
        if (pa >= 0) free_pipe_if_unused((unsigned)pa);
        if (pb >= 0) free_pipe_if_unused((unsigned)pb);
        return -24;
    }
    Open a{};
    a.used = true; a.upair = true; a.tx = (unsigned)pa; a.rx = (unsigned)pb;
    Open b{};
    b.used = true; b.upair = true; b.tx = (unsigned)pb; b.rx = (unsigned)pa;
    set->open[i0] = a;
    set->open[i1] = b;
    *(uint32_t *)sv = FD_BASE + i0;
    *(uint32_t *)(sv + 4) = FD_BASE + i1;
    return 0;
}

// --- internet + Unix sockets -------------------------------------------------
long do_socket(uint64_t domain, uint64_t type, uint64_t proto) {
    (void)proto;
    if (type != 1) return -94; // ESOCKTNOSUPPORT (stream only, for now)
    if (domain != 2 && domain != 1) return -97; // AF_INET or AF_UNIX
    FdSet *set = fds_current();
    for (unsigned i = 0; i < FD_N; i++) {
        if (set->open[i].used) continue;
        set->open[i] = Open{};
        set->open[i].used = true;
        if (domain == 2) {
            set->open[i].is_sock = true;
            set->open[i].sock = -1;
        } else {
            set->open[i].is_unix = true;
            set->open[i].ulist = -1;
        }
        return (long)(FD_BASE + i);
    }
    return -24; // EMFILE
}

long do_connect(uint64_t fd, uint64_t addr, uint64_t len) {
    Open *o = fd_get(fd);
    if (!o) return -88; // ENOTSOCK
    if (o->is_unix) {
        char path[72];
        long r = unix_read_path(addr, path, sizeof(path));
        if (r < 0) return r;
        int li = unix_find(path, true);
        if (li < 0) return -111; // ECONNREFUSED: no listener at that path
        int qs = -1;
        for (unsigned k = 0; k < 4; k++)
            if (!s_unix[li].q[k].used) { qs = (int)k; break; }
        if (qs < 0) return -111; // backlog full
        int pa = alloc_pipe(), pb = alloc_pipe();
        if (pa < 0 || pb < 0) {
            if (pa >= 0) free_pipe_if_unused((unsigned)pa);
            if (pb >= 0) free_pipe_if_unused((unsigned)pb);
            return -24;
        }
        // Client end writes into pa (the server's rx) and reads from pb.
        o->upair = true;
        o->tx = (unsigned)pa;
        o->rx = (unsigned)pb;
        s_unix[li].q[qs].used = true;
        s_unix[li].q[qs].tx = (unsigned)pb; // server writes into pb
        s_unix[li].q[qs].rx = (unsigned)pa; // server reads from pa
        return 0;
    }
    if (!o->is_sock) return -88;
    if (!user_range_ok(addr, 16)) return -14;
    const uint8_t *sa = (const uint8_t *)addr;
    if (sa[0] != 2 || sa[1] != 0) return -97; // AF_INET only
    uint16_t port = (uint16_t)((sa[2] << 8) | sa[3]); // network order
    uint32_t ip = (uint32_t)sa[4] | ((uint32_t)sa[5] << 8) | ((uint32_t)sa[6] << 16) |
                  ((uint32_t)sa[7] << 24);
    (void)len;
    // The SYSCALL entry masks interrupts; the connect wait needs the PIT (for
    // its timeout) and the NIC RX IRQ to make progress, so enable them here.
    choco::cpu::sti();
    int h = net::tcp::connect(ip, port, 10000);
    if (h < 0) return -111; // ECONNREFUSED / ETIMEDOUT
    o->sock = h;
    return 0;
}

long do_getsockname(uint64_t fd, uint64_t addr) {
    Open *o = fd_get(fd);
    if (!o || !o->is_sock) return -88;
    if (!user_range_ok(addr, 16)) return -14;
    uint8_t *sa = (uint8_t *)addr;
    for (int i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = 2; // AF_INET
    return 0;
}

// bind(fd, sockaddr, len): reserve a local address for a later listen().
long do_bind(uint64_t fd, uint64_t addr, uint64_t len) {
    (void)len;
    Open *o = fd_get(fd);
    if (!o) return -88;
    if (o->is_unix) {
        char path[72];
        long r = unix_read_path(addr, path, sizeof(path));
        if (r < 0) return r;
        if (!path[0]) return -22; // EINVAL: empty sun_path
        if (unix_find(path, false) >= 0) return -98; // EADDRINUSE
        int li = unix_alloc();
        if (li < 0) return -23; // ENFILE: no free endpoint slot
        for (size_t i = 0; i < sizeof(s_unix[li].path); i++) s_unix[li].path[i] = path[i];
        s_unix[li].used = true;
        s_unix[li].listening = false;
        o->ulist = li;
        return 0;
    }
    if (!o->is_sock) return -88;
    if (!user_range_ok(addr, 16)) return -14;
    const uint8_t *sa = (const uint8_t *)addr;
    if (sa[0] != 2 || sa[1] != 0) return -97; // AF_INET only
    o->b_port = (uint16_t)((sa[2] << 8) | sa[3]);
    o->b_ip = (uint32_t)sa[4] | ((uint32_t)sa[5] << 8) | ((uint32_t)sa[6] << 16) |
              ((uint32_t)sa[7] << 24);
    return 0;
}

// listen(fd, backlog): turn a bound socket into a listener.
long do_listen(uint64_t fd, uint64_t backlog) {
    Open *o = fd_get(fd);
    if (!o) return -88;
    if (o->is_unix) {
        if (o->ulist < 0) return -22; // must bind() first
        s_unix[o->ulist].listening = true;
        (void)backlog; // the 4-slot queue is a fixed backlog
        return 0;
    }
    if (!o->is_sock) return -88;
    if (!o->b_port) return -22; // must bind() first
    int h = net::tcp::listen(o->b_ip, o->b_port, (int)backlog);
    if (h < 0) return -23; // ENFILE
    o->sock = h;
    return 0;
}

// accept(fd, sockaddr, len): pop a completed inbound connection into a new fd.
long do_accept(uint64_t fd, uint64_t addr, uint64_t len) {
    (void)len;
    Open *o = fd_get(fd);
    if (!o) return -88;
    if (o->is_unix) {
        if (o->ulist < 0 || !s_unix[o->ulist].listening) return -22;
        UnixSock &u = s_unix[o->ulist];
        int qs = -1;
        for (unsigned k = 0; k < 4; k++)
            if (u.q[k].used) { qs = (int)k; break; }
        if (qs < 0) return -11; // EAGAIN: no pending connection
        FdSet *set = fds_current();
        for (unsigned i = 0; i < FD_N; i++) {
            if (set->open[i].used) continue;
            set->open[i] = Open{};
            set->open[i].used = true;
            set->open[i].upair = true;
            set->open[i].tx = u.q[qs].tx;
            set->open[i].rx = u.q[qs].rx;
            u.q[qs].used = false;
            if (addr && user_range_ok(addr, 16)) {
                uint8_t *sa = (uint8_t *)addr;
                for (int k = 0; k < 16; k++) sa[k] = 0;
                sa[0] = 1; // AF_UNIX (peer path not filled)
            }
            return (long)(FD_BASE + i);
        }
        return -24; // EMFILE: leave the pending connection queued
    }
    if (!o->is_sock || o->sock < 0) return -88;
    int c = net::tcp::accept(o->sock, 30000);
    if (c < 0) return -11; // EAGAIN
    FdSet *set = fds_current();
    for (unsigned i = 0; i < FD_N; i++) {
        if (set->open[i].used) continue;
        set->open[i] = Open{};
        set->open[i].used = true;
        set->open[i].is_sock = true;
        set->open[i].sock = c;
        if (addr && user_range_ok(addr, 16)) {
            uint8_t *sa = (uint8_t *)addr;
            for (int k = 0; k < 16; k++) sa[k] = 0;
            sa[0] = 2; // AF_INET (peer address not filled)
        }
        return (long)(FD_BASE + i);
    }
    net::tcp::close(c);
    return -24; // EMFILE
}

// --- poll/select readiness -------------------------------------------------
bool fd_readable(uint64_t fd) {
    Open *o = fd_get(fd);
    if (!o) return fd == 0 && tty::cooked_count() > 0; // console tty input
    if (o->is_sock) return o->sock >= 0 && net::tcp::readable(o->sock);
    if (o->upair) return s_pipe[o->rx].count > 0;
    if (o->is_unix) return unix_has_pending(o->ulist); // listening socket
    if (o->pipe) return s_pipe[o->pipe_id].count > 0;
    if (o->writable >= 0) return false; // write-only file
    return o->off < o->size;            // file with bytes left to read
}
bool fd_writable(uint64_t fd) {
    Open *o = fd_get(fd);
    if (!o) return fd <= 2; // console is always writable
    if (o->is_sock) return o->sock >= 0 && net::tcp::is_open(o->sock);
    if (o->upair) return true;
    if (o->is_unix) return false; // a listening socket is not writable
    if (o->pipe) return true;
    return o->writable >= 0;
}
// poll(fds, nfds, timeout_ms): struct pollfd { int fd; short events; short revents; }
long do_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms) {
    if (nfds > 64) return -22;
    if (nfds && !user_range_ok(fds, nfds * 8)) return -14;
    const bool infinite = (timeout_ms == (uint64_t)-1);
    uint64_t deadline = pit::ticks() + (infinite ? 0 : timeout_ms * pit::HZ / 1000);
    for (;;) {
        long ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            uint8_t *p = (uint8_t *)(fds + i * 8);
            const int fd = *(int *)p;
            const uint16_t events = *(uint16_t *)(p + 4);
            uint16_t revents = 0;
            if ((events & 1) && fd_readable((uint64_t)fd)) revents |= 1;  // POLLIN
            if ((events & 4) && fd_writable((uint64_t)fd)) revents |= 4;  // POLLOUT
            *(uint16_t *)(p + 6) = revents;
            if (revents) ready++;
        }
        if (ready) return ready;
        if (timeout_ms == 0) return 0;
        if (!infinite && (int64_t)(pit::ticks() - deadline) >= 0) return 0;
        net::poll();
        sched::sleep_ms(10);
    }
}

inline bool fdset_test(uint64_t set, uint64_t fd) {
    return ((const uint8_t *)set)[fd >> 3] & (uint8_t)(1u << (fd & 7));
}
inline void fdset_put(uint64_t set, uint64_t fd, bool v) {
    if (!set) return;
    uint8_t *p = (uint8_t *)set;
    if (v) p[fd >> 3] |= (uint8_t)(1u << (fd & 7));
    else p[fd >> 3] &= (uint8_t)~(1u << (fd & 7));
}

// select(nfds, readfds, writefds, exceptfds, timeout): fd_set is 128 bytes.
long do_select(uint64_t nfds, uint64_t rfds, uint64_t wfds, uint64_t efds, uint64_t tv) {
    if (nfds > 1024) return -22;
    if ((rfds && !user_range_ok(rfds, 128)) || (wfds && !user_range_ok(wfds, 128)) ||
        (efds && !user_range_ok(efds, 128)))
        return -14;
    bool infinite = true;
    uint64_t timeout_ms = 0;
    if (tv) {
        if (!user_range_ok(tv, 16)) return -14;
        const uint64_t sec = *(const uint64_t *)tv, usec = *(const uint64_t *)(tv + 8);
        infinite = false;
        timeout_ms = sec * 1000 + usec / 1000;
    }
    uint64_t deadline = pit::ticks() + (infinite ? 0 : timeout_ms * pit::HZ / 1000);
    for (;;) {
        long ready = 0;
        for (uint64_t fd = 0; fd < nfds; fd++) {
            const bool want_r = rfds && fdset_test(rfds, fd);
            const bool want_w = wfds && fdset_test(wfds, fd);
            const bool r = want_r && fd_readable(fd);
            const bool w = want_w && fd_writable(fd);
            fdset_put(rfds, fd, r);
            fdset_put(wfds, fd, w);
            fdset_put(efds, fd, false);
            if (r || w) ready++;
        }
        if (ready) return ready;
        if (!infinite && timeout_ms == 0) return 0;
        if (!infinite && (int64_t)(pit::ticks() - deadline) >= 0) return 0;
        net::poll();
        sched::sleep_ms(10);
    }
}

long do_dup2(uint64_t oldfd, uint64_t newfd) {
    Open *old = fd_get(oldfd);
    if (!old || newfd >= FD_BASE + FD_N) return -9;
    if (oldfd == newfd) return (long)newfd;
    FdSet *set = fds_current();
    if (newfd < FD_BASE) set->std[newfd] = *old;
    else set->open[newfd - FD_BASE] = *old;
    return (long)newfd;
}

long do_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    Open *o = fd_get(fd);
    if (!o) return -9;
    size_t base = 0;
    if (whence == 0) base = 0;
    else if (whence == 1) base = o->off;
    else if (whence == 2) base = o->size;
    else return -22; // EINVAL
    size_t npos = base + (size_t)off; // off is non-negative (no shrink)
    if (npos > o->size) return -22;
    o->off = npos;
    return (long)npos;
}

long do_fsstat(uint64_t idx, uint64_t buf, uint64_t cap) {
    const char *name = nullptr;
    size_t size = 0;
    if (!fs::stat((size_t)idx, &name, &size)) return -2;
    if (buf == 0 || cap == 0) return (long)size; // size probe
    if (cap > 256) cap = 256;
    if (!user_range_ok(buf, cap)) return -14;
    size_t n = 0;
    while (name[n] && n + 1 < cap) ((char *)buf)[n] = name[n], n++;
    ((char *)buf)[n] = 0;
    return (long)size;
}

} // namespace

void reset_files() { fds_reset(); }
void reset_files_for(uint32_t pid) { s_fds[pid & (FD_CTX_N - 1)] = FdSet{}; }
void set_tty_pgid(uint32_t pgid) {
    s_tty_pgid = pgid ? pgid : 1;
    tty::set_fg_pgid(s_tty_pgid);
}

void set_std_out(uint32_t pid, long writable_id) {
    FdSet *set = &s_fds[pid & (FD_CTX_N - 1)];
    set->std[1] = Open{};
    set->std[1].used = true;
    set->std[1].writable = (int)writable_id;
    set->std[1].off = 0;
    set->std[2] = set->std[1];
}

void set_std_in(uint32_t pid, const void *data, size_t size) {
    FdSet *set = &s_fds[pid & (FD_CTX_N - 1)];
    set->std[0] = Open{};
    if (data) {
        set->std[0].used = true;
        set->std[0].data = (const uint8_t *)data;
        set->std[0].size = size;
        set->std[0].off = 0;
    }
}

void clone_files(uint32_t child_pid) {
    FdSet parent = *fds_current();
    s_fds[child_pid & (FD_CTX_N - 1)] = parent;
}

void drop_files(uint32_t pid) {
    s_fds[pid & (FD_CTX_N - 1)] = FdSet{};
}

long f_size(uint64_t fd) {
    Open *o = fd_get(fd);
    return o ? (long)o->size : -9;
}

bool user_ok(uint64_t ptr, uint64_t len) { return user_range_ok(ptr, len); }

// --- Linux x86_64 translator (global: any ring-3 program may use it) ---
namespace linux_abi {
namespace {

// Fill a Linux struct stat (144 bytes): zeroed, regular file, size set.
 long fill_stat(uint64_t buf, long size, bool directory = false) {
    if (!user_range_ok(buf, 144)) return -14;
    uint8_t *s = (uint8_t *)buf;
    for (unsigned i = 0; i < 144; i++) s[i] = 0;
    *(uint32_t *)(s + 24) = directory ? 0040000 : 0100000; // S_IFDIR/S_IFREG
    *(uint32_t *)(s + 28) = 0;            // uid
    *(uint32_t *)(s + 32) = 0;            // gid
    *(uint64_t *)(s + 16) = directory ? 2 : 1; // nlink
    *(uint64_t *)(s + 48) = (uint64_t)size; // st_size
    *(uint64_t *)(s + 56) = 4096;           // st_blksize
    *(uint64_t *)(s + 64) = directory ? 0 : (uint64_t)((size + 511) / 512);
    uint64_t now = choco::rtc::now(); // real wall clock, not uptime
    *(uint64_t *)(s + 72) = now;             // atime sec
    *(uint64_t *)(s + 88) = now;             // mtime sec
    *(uint64_t *)(s + 104) = now;            // ctime sec
    return 0;
}

long do_stat_path(uint64_t path, uint64_t buf) {
    if (!user_cstr_ok(path)) return -14;
    bool dir = fs::writable_is_dir((const char *)path);
    const void *data = nullptr; size_t size = 0;
    if (!dir && !fs::read((const char *)path, &data, &size)) return -2;
    return fill_stat(buf, dir ? -1 : (long)size, dir);
}

long do_uname(uint64_t buf) {
    if (!user_range_ok(buf, 6 * 65)) return -14;
    uint8_t *s = (uint8_t *)buf;
    for (unsigned i = 0; i < 6 * 65; i++) s[i] = 0;
    const char *f[6] = {"Linux", "choco", "5.4.0", "#1 ChocoOS compat", "x86_64", ""};
    for (unsigned i = 0; i < 6; i++) {
        unsigned k = 0;
        while (f[i][k] && k < 64) { s[i * 65 + k] = (uint8_t)f[i][k]; k++; }
    }
    return 0;
}

long do_clock_gettime(uint64_t clockid, uint64_t buf) {
    if (!user_range_ok(buf, 16)) return -14;
    if (clockid == 0) { // CLOCK_REALTIME: real wall clock from the RTC
        *(uint64_t *)buf = choco::rtc::now();
        *(uint64_t *)(buf + 8) = 0;
        return 0;
    }
    // CLOCK_MONOTONIC (1) and the rest: uptime.
    uint64_t t = pit::ticks();
    *(uint64_t *)buf = t / pit::HZ;
    *(uint64_t *)(buf + 8) = (t % pit::HZ) * (1000000000ULL / pit::HZ);
    return 0;
}

long do_gettimeofday(uint64_t tv, uint64_t tz) {
    if (!user_range_ok(tv, 16)) return -14;
    uint64_t t = pit::ticks();
    /* struct timeval: tv_sec (8) + tv_usec (8) at offset 0-15 */
    *(uint64_t *)tv = choco::rtc::now();     // real wall clock (RTC + uptime)
    *(uint64_t *)(tv + 8) = (t % pit::HZ) * (1000000ULL / pit::HZ); // microseconds
    if (tz) {
        if (!user_range_ok(tz, 8)) return -14;
        /* struct timezone: tz_minuteswest (4) + tz_dsttime (4) at offset 0-7 */
        *(int *)tz = 0;
        *(int *)(tz + 4) = 0;
    }
    return 0;
}

long do_settimeofday(uint64_t tv, uint64_t tz) {
    // For v1: accept the values but keep running on PIT; in a full impl
    // one would set the real RTC. For now just return success.
    (void)tv;
    (void)tz;
    return 0;
}

long do_vec(uint64_t fd, uint64_t iov, uint64_t cnt, bool is_write) {
    if (cnt > 32) return -22;
    if (!user_range_ok(iov, cnt * 16)) return -14;
    long total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t base = *(uint64_t *)(iov + i * 16);
        uint64_t len = *(uint64_t *)(iov + i * 16 + 8);
        long r = is_write ? do_write(fd, base, len) : do_read(fd, base, len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r != len) break; // short counts stop the vector
    }
    return total;
}

long do_openat(uint64_t dirfd, uint64_t path, uint64_t flags) {
    if (dirfd != (uint64_t)-100 && (int64_t)dirfd < 0) return -9;
    return do_open(path, flags);
}

long do_unlink(uint64_t path) {
    if (!user_cstr_ok(path)) return -14;
    if (fs::writable_unlink((const char *)path)) {
        fs::disk_persistence_flush();
        return 0;
    }
    const void *d = nullptr; size_t n = 0;
    return fs::read((const char *)path, &d, &n) ? -30 : -2;
}

long do_rename(uint64_t old_path, uint64_t new_path) {
    if (!user_cstr_ok(old_path) || !user_cstr_ok(new_path)) return -14;
    if (fs::writable_rename((const char *)old_path, (const char *)new_path)) {
        fs::disk_persistence_flush();
        return 0;
    }
    const void *d = nullptr; size_t n = 0;
    return fs::read((const char *)old_path, &d, &n) ? -30 : -2;
}

long do_mkdir(uint64_t path) {
    if (!user_cstr_ok(path)) return -14;
    if (fs::writable_mkdir((const char *)path)) {
        fs::disk_persistence_flush();
        return 0;
    }
    return fs::writable_is_dir((const char *)path) ? -17 : -28;
}

long do_rmdir(uint64_t path) {
    if (!user_cstr_ok(path)) return -14;
    if (fs::writable_rmdir((const char *)path)) {
        fs::disk_persistence_flush();
        return 0;
    }
    return -2;
}

long do_access(uint64_t path) {
    if (!user_cstr_ok(path)) return -14;
    if (fs::writable_is_dir((const char *)path)) return 0;
    const void *d = nullptr;
    size_t sz = 0;
    return fs::read((const char *)path, &d, &sz) ? 0 : -2;
}

long do_getrandom(uint64_t buf, uint64_t len) {
    if (len > 256) len = 256;
    if (!user_range_ok(buf, len)) return -14;
    // NOT crypto-safe (PIT ticks): enough for SSP canaries in v1.
    uint64_t s = pit::ticks() * 6364136223846793005ULL + 1442695040888963407ULL;
    for (uint64_t i = 0; i < len; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        ((uint8_t *)buf)[i] = (uint8_t)(s >> 33);
    }
    return (long)len;
}

long do_nanosleep(uint64_t req) {
    if (!user_range_ok(req, 16)) return -14;
    uint64_t sec = *(uint64_t *)req, nsec = *(uint64_t *)(req + 8);
    if (nsec >= 1000000000ULL) return -22;
    uint64_t ms = sec * 1000 + nsec / 1000000ULL;
    if (ms > 60000) ms = 60000;
    sched::sleep_ms(ms);
    return 0;
}

long do_fcntl(uint64_t fd, uint64_t cmd) {
    if (!fd_get(fd) && fd > 2) return -9;
    if (cmd == 0) return 0; // F_DUPFD (minimal)
    if (cmd == 1 || cmd == 2) return 0; // FD flags
    if (cmd == 3) return 0; // F_GETFL -> O_RDONLY
    if (cmd == 4) return 0; // F_SETFL -> ignored
    return -22;
}

long do_getrlimit(uint64_t buf) {
    if (!user_range_ok(buf, 16)) return -14;
    *(uint64_t *)buf = (uint64_t)-1; // RLIM_INFINITY
    *(uint64_t *)(buf + 8) = (uint64_t)-1;
    return 0;
}

} // namespace
} // namespace linux_abi

void handle(uint64_t vec, void *frame_ptr) {    (void)vec;
    auto *f = (cpu::Frame *)frame_ptr;
    uint64_t n = f->rax;
    long ret = -38; // ENOSYS
    switch (n) {
    case N_EXIT:
        // Close only the exiting task's descriptors: clearing every table here
        // would rip open files out from under another running job.
        reset_files_for(sched::current_pid());
        proc::exit_current((long)f->rdi); // never returns
        break;
    case N_WRITE: ret = do_write(f->rdi, f->rsi, f->rdx); break;
    case N_READ: ret = do_read(f->rdi, f->rsi, f->rdx); break;
    case N_SLEEP: sched::sleep_ms(f->rdi); ret = 0; break;
    case N_TICKS: ret = (long)pit::ticks(); break;
    case N_YIELD: sched::yield(); ret = 0; break;
    case N_OPEN: ret = do_open(f->rdi, f->rsi); break;
    case N_CLOSE: ret = do_close(f->rdi); break;
    case N_SBRK: ret = (long)proc::proc_sbrk((int64_t)f->rdi); break;
    case N_LSEEK: ret = do_lseek(f->rdi, f->rsi, f->rdx); break;
    case N_FSIZE: {
        Open *o = fd_get(f->rdi);
        ret = o ? (long)o->size : -9;
        break;
    }
    case N_FSCOUNT: ret = (long)fs::count(); break;
    case N_FSSTAT: ret = do_fsstat(f->rdi, f->rsi, f->rdx); break;
    case N_WIN_CREATE: {
        uint64_t addr = 0;
        ret = gui::user_create(f->rdi, f->rsi, (const char *)f->rdx, &addr);
        if (ret == 0) ret = (long)addr;
        break;
    }
    case N_WIN_PRESENT: ret = gui::user_present(f->rdi); break;
    case N_WIN_CLOSE: ret = gui::user_close(f->rdi); break;
    case N_WIN_RESIZE: ret = gui::user_resize(f->rdi, f->rsi, f->rdx); break;
    case N_MKDIR:
        if (!user_cstr_ok(f->rdi)) { ret = -14; break; }
        if (fs::writable_mkdir((const char *)f->rdi)) { fs::disk_persistence_flush(); ret = 0; }
        else ret = fs::writable_is_dir((const char *)f->rdi) ? -17 : -2;
        break;
    case N_RMDIR:
        if (!user_cstr_ok(f->rdi)) { ret = -14; break; }
        if (fs::writable_rmdir((const char *)f->rdi)) { fs::disk_persistence_flush(); ret = 0; }
        else ret = -2;
        break;
    case N_IOCTL: ret = do_ioctl(f->rdi, f->rsi, f->rdx); break;
    case N_UNLINK:
        if (!user_cstr_ok(f->rdi)) { ret = -14; break; }
        if (fs::writable_unlink((const char *)f->rdi)) { fs::disk_persistence_flush(); ret = 0; }
        else ret = -2;
        break;
    case N_RENAME:
        if (!user_cstr_ok(f->rdi) || !user_cstr_ok(f->rsi)) { ret = -14; break; }
        if (fs::writable_rename((const char *)f->rdi, (const char *)f->rsi)) {
            fs::disk_persistence_flush();
            ret = 0;
        } else ret = -2;
        break;
    default: break;
    }
    f->rax = (uint64_t)ret; // popped into rax by the stub -> user return value
    proc::deliver_pending(f);
}

// Linux x86_64 syscall entry from kernel/cpu/linux.asm. Args follow the
// Linux convention (rdi,rsi,rdx,r10,r8,r9); errors are -errno like Linux.
// C linkage so linux.asm can call it; lives in choco::syscall to reuse
// the file/pointer validators above.
extern "C" long linux_dispatch(choco::cpu::Frame *fp) {
    auto *f = fp;
    uint64_t n = f->rax;
    long ret = -38; // ENOSYS
    switch (n) {
    case 0: ret = do_read(f->rdi, f->rsi, f->rdx); break;
    case 1: ret = do_write(f->rdi, f->rsi, f->rdx); break;
    case 3: ret = do_close(f->rdi); break;
    case 4: ret = linux_abi::do_stat_path(f->rdi, f->rsi); break; // stat
    case 5: { // fstat(fd, statbuf): statbuf is RSI (was wrongly filled at RDI=fd)
        Open *o = fd_get(f->rdi);
        ret = o ? linux_abi::fill_stat(f->rsi, (long)o->size) : -9;
        break;
    }
    case 6: ret = linux_abi::do_stat_path(f->rdi, f->rsi); break; // lstat
    case 8: ret = do_lseek(f->rdi, f->rsi, f->rdx); break;
    case 9: { // mmap(addr,len,prot,flags,fd,off)
        const uint64_t prot = f->rdx, flags = f->r10;
        const int fd = (int)f->r8;
        const uint64_t off = f->r9;
        if (!(flags & 0x02)) { ret = -22; break; } // only MAP_PRIVATE
        if (flags & 0x20) {                        // MAP_ANONYMOUS
            ret = (long)linux::mmap(f->rdi, f->rsi);
        } else {                                   // file-backed
            Open *o = fd_get((uint64_t)fd);
            if (!o || !o->data || o->is_sock) { ret = -9; break; }
            if (off % 4096) { ret = -22; break; }  // page-aligned offset only
            ret = (long)linux::mmap_file(f->rdi, f->rsi, o->data, o->size, off);
        }
        if (ret > 0) linux::mprotect((uint64_t)ret, f->rsi, prot); // honor PROT_*
        break;
    }
    case 10: ret = linux::mprotect(f->rdi, f->rsi, f->rdx); break; // mprotect
    case 11: ret = linux::munmap(f->rdi, f->rsi); break;
    case 12: ret = linux::brk(f->rdi); break;
    case 13: ret = proc::linux_sigaction(f->rdi, f->rsi, f->rdx); break;
    case 14: ret = proc::linux_sigprocmask(f->rdi, f->rsi, f->rdx); break;
    case 15: ret = proc::linux_sigreturn(f); break;
    case 16: ret = do_ioctl(f->rdi, f->rsi, f->rdx); break;
    case 19: ret = linux_abi::do_vec(f->rdi, f->rsi, f->rdx, false); break; // readv
    case 20: ret = linux_abi::do_vec(f->rdi, f->rsi, f->rdx, true); break;  // writev
    case 21: ret = linux_abi::do_access(f->rdi); break;
    case 82: ret = linux_abi::do_rename(f->rdi, f->rsi); break;
    case 83: ret = linux_abi::do_mkdir(f->rdi); break;
    case 84: ret = linux_abi::do_rmdir(f->rdi); break;
    case 87: ret = linux_abi::do_unlink(f->rdi); break;
    case 22: ret = do_pipe(f->rdi, 0); break; // pipe
    case 33: ret = do_dup2(f->rdi, f->rsi); break;
    case 24: sched::yield(); ret = 0; break; // sched_yield
    case 25: ret = linux_abi::do_gettimeofday(f->rdi, f->rsi); break; // gettimeofday
    case 26: ret = linux_abi::do_settimeofday(f->rdi, f->rsi); break; // settimeofday
    case 35: ret = linux_abi::do_nanosleep(f->rdi); break;
    // Internet sockets (client-only over the kernel TCP engine).
    case 41: ret = do_socket(f->rdi, f->rsi, f->rdx); break;  // socket
    case 42: ret = do_connect(f->rdi, f->rsi, f->rdx); break; // connect
    case 43: ret = do_accept(f->rdi, f->rsi, f->rdx); break; // accept
    case 44: ret = do_write(f->rdi, f->rsi, f->rdx); break;   // sendto (connected stream)
    case 45: ret = do_read(f->rdi, f->rsi, f->rdx); break;    // recvfrom (connected stream)
    case 46:                                                  // sendmsg
    case 47: ret = -38; break;                                // recvmsg: ENOSYS
    case 48: ret = 0; break;                                  // shutdown (close does the work)
    case 49: ret = do_bind(f->rdi, f->rsi, f->rdx); break;    // bind
    case 50: ret = do_listen(f->rdi, f->rsi); break;          // listen
    case 51:                                                  // getsockname
    case 52: ret = do_getsockname(f->rdi, f->rsi); break;     // getpeername
    case 54:                                                  // setsockopt
    case 55: ret = 0; break;                                  // getsockopt: accepted
    case 288: ret = do_accept(f->rdi, f->rsi, f->rdx); break;  // accept4
    case 53: ret = do_socketpair(f->rdi, f->rsi, f->rdx, f->r10); break; // socketpair
    case 7: ret = do_poll(f->rdi, f->rsi, f->rdx); break;     // poll
    case 23: ret = do_select(f->rdi, f->rsi, f->rdx, f->r10, f->r8); break; // select
    case 39: ret = (long)proc::proc_pid(); break; // getpid
    case 56: ret = proc::clone_current(f->rdi, f->rsi, f); break;
    case 59: ret = proc::exec_current(f->rdi, f->rsi, f); break;
    case 61: {
        // wait4(pid, status, options, rusage): status is RSI, not RDX.
        if (f->rsi && !user_range_ok(f->rsi, 4)) ret = -14;
        else ret = sched::wait_child((int64_t)f->rdi, f->rsi);
        break;
    }
    case 60:                  // exit
    case 231:                 // exit_group
        proc::exit_current((long)f->rdi); // never returns
        break;
    case 62:  // kill
    case 200: // tkill
    case 234: { // tgkill(tgid, tid, sig): fatal signals terminate (no handlers v1)
        int64_t target = (int64_t)f->rdi;
        uint64_t sig = (n == 234) ? f->rdx : f->rsi;
        if (sig == 2 || sig == 9 || sig == 6 || sig == 15) {
            if (sig != 9 && proc::linux_signal_ignored(sig)) { ret = 0; break; }
            if (target == (int64_t)proc::proc_pid() && proc::linux_deliver_signal(sig, f)) {
                ret = 0;
                break;
            }
            if (target == (int64_t)proc::proc_pid())
                proc::exit_current(128 + (long)sig); // current task: full cleanup
            if (target == 0) {
                ret = sched::signal_group(sched::current_pgid(), (uint32_t)sig) ? 0 : -3;
                break;
            }
            if (target < 0) {
                ret = sched::signal_group((uint32_t)-target, (uint32_t)sig) ? 0 : -3;
                break;
            }
            ret = sched::signal_pid((uint32_t)target, (uint32_t)sig) ? 0 : -3;
            break;
        } else if (sig == 17) {
            // SIGCHLD is generated by the zombie/wait model; it is accepted
            // here so libc can probe or ignore it without ENOSYS.
            ret = 0;
        } else {
            ret = -22; // unsupported signal number
        }
        break;
    }
    case 63: ret = linux_abi::do_uname(f->rdi); break;
    case 72: ret = linux_abi::do_fcntl(f->rdi, f->rsi); break; // fcntl(fd,cmd,...)
    case 97: ret = linux_abi::do_getrlimit(f->rdi); break;
    case 102: ret = 0; break;  // getuid
    case 104: ret = 0; break;  // getgid
    case 107: ret = 0; break;  // geteuid
    case 108: ret = 0; break;  // getegid
    case 110: ret = (long)proc::proc_ppid(); break; // getppid
    case 111: ret = (long)sched::current_pgid(); break; // getpgrp
    case 121: ret = (long)sched::current_pgid(); break; // getpgid(0)
    case 109: { // setpgid(pid, pgid)
        uint32_t pid = f->rdi ? (uint32_t)f->rdi : proc::proc_pid();
        uint32_t pgid = f->rsi ? (uint32_t)f->rsi : pid;
        ret = sched::set_pgid(pid, pgid) ? 0 : -3;
        break;
    }
    case 158: { // arch_prctl
        if (f->rdi == 0x1002) ret = linux::set_fs(f->rsi);        // SET_FS
        else if (f->rdi == 0x1003) {                              // GET_FS
            if (!user_range_ok(f->rsi, 8)) ret = -14;
            else { *(uint64_t *)f->rsi = linux::get_fs(); ret = 0; }
        } else ret = -22;
        break;
    }
    case 201: ret = (long)choco::rtc::now(); break; // time (real wall clock)
    case 218: ret = 1; break; // set_tid_address: fake TID (no threads to wake)
    case 221: ret = 0; break; // fadvise64: nothing to advise in v1
    case 228: ret = linux_abi::do_clock_gettime(f->rdi, f->rsi); break; // clock_gettime(clock,ts)
    case 257: ret = linux_abi::do_openat(f->rdi, f->rsi, f->rdx); break;
    case 262: { // newfstatat(dirfd,path,statbuf,flags): path-based, read-only
        if (f->rdi != (uint64_t)-100 && (int64_t)f->rdi < 0) { ret = -9; break; }
        if (!user_cstr_ok(f->rsi)) { ret = -14; break; }
        const void *d = nullptr;
        size_t sz = 0;
        bool is_dir = fs::writable_is_dir((const char *)f->rsi);
        if (!is_dir && !fs::read((const char *)f->rsi, &d, &sz)) ret = -2;
        else ret = linux_abi::fill_stat(f->rdx, is_dir ? -1 : (long)sz, is_dir);
        break;
    }
    case 273: ret = 0; break;  // set_robust_list (alt number on some tables)
    case 292: ret = f->rdx ? -22 : do_dup2(f->rdi, f->rsi); break; // dup3
    case 293: ret = do_pipe(f->rdi, f->rsi); break; // pipe2
    case 318: ret = linux_abi::do_getrandom(f->rdi, f->rsi); break;
    case 334: ret = -38; break; // rseq: musl tolerates ENOSYS
    case 202: ret = -38; break; // futex: no contention possible, no threads
    default: break;
    }
    f->rax = (uint64_t)ret;
    proc::deliver_pending(f);
    return ret;
}

} // namespace choco::syscall

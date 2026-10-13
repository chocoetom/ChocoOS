// Interactive shell: line editing, history, built-in commands.
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/fat32.hpp>
#include <choco/gui.hpp>
#include <choco/rtc.hpp>
#include <choco/mem.hpp>
#include <choco/mouse.hpp>
#include <choco/net.hpp>
#include <choco/pci.hpp>
#include <choco/sched.hpp>
#include <choco/shell.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

namespace con = choco::console;
using con::ACC; using con::BG; using con::FG; using con::OK; using con::ERR;

namespace choco::shell {
namespace {

constexpr const char *TITLE = "ChocoOS 0.5 - powered by Choco the black cat";
constexpr size_t LINE_MAX = 256;
constexpr size_t HIST_N = 16;
constexpr size_t ARGS_MAX = 8;

char s_line[LINE_MAX]{};
size_t s_len = 0;
char s_hist[HIST_N][LINE_MAX]{};
size_t s_hist_count = 0;
size_t s_hist_pos = 0;
bool s_browsing = false;
char s_saved[LINE_MAX]{};

// Command pipeline state (Tier 2): exit status for $?, and the redirection
// targets resolved by exec() for the command currently being dispatched.
int s_last_status = 0;                                    // $?
char s_rout[200];                                         // '>' / '>>' target
bool s_has_out = false, s_rapp = false;
char s_rin[200];                                          // '<' source
bool s_has_in = false;
bool s_bg = false; // the current job is backgrounded with '&'

struct Counter { const char *name; volatile uint64_t n = 0; };
Counter s_ca{"A"}, s_cb{"B"};

void counter_main(void *arg) {
    auto *c = (Counter *)arg;
    // Demo threads for the status bar: tick once, then sleep. A tight loop here
    // burned CPU on the single core and starved the GUI renderer/cursor.
    for (;;) {
        c->n = c->n + 1;
        sched::sleep_ms(200);
    }
}

void prompt() { con::print("choco> ", ACC); }

void show_text(const char *s) {
    size_t n = util::strlen(s);
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    util::memcpy(s_line, s, n);
    s_line[n] = 0;
    s_len = n;
    con::print(s_line);
}

void erase_echo() {
    while (s_len) { con::backspace(); s_len--; }
    s_line[0] = 0;
}

void history_push(const char *line) {
    if (s_hist_count < HIST_N) {
        util::memcpy(s_hist[s_hist_count], line, LINE_MAX);
        s_hist_count++;
    } else {
        for (size_t i = 0; i + 1 < HIST_N; i++)
            util::memcpy(s_hist[i], s_hist[i + 1], LINE_MAX);
        util::memcpy(s_hist[HIST_N - 1], line, LINE_MAX);
    }
}

void recall(int dir) { // +1 = older (up), -1 = newer (down)
    if (!s_hist_count) return;
    if (!s_browsing) {
        util::memcpy(s_saved, s_line, s_len + 1);
        s_hist_pos = s_hist_count;
        s_browsing = true;
    }
    if (dir > 0 && s_hist_pos > 0) s_hist_pos--;
    else if (dir < 0 && s_hist_pos < s_hist_count) s_hist_pos++;
    else return;
    erase_echo();
    show_text(s_hist_pos == s_hist_count ? s_saved : s_hist[s_hist_pos]);
}

// --- commands ---

using CmdFn = void (*)(int argc, char **argv);

void c_help(int, char **) {
    static const char *kLines[] = {
        "  help                 show this list",
        "  clear                clear the screen (Ctrl+L)",
        "  mem                  memory, heap and thread summary",
        "  uptime               seconds and ticks since boot",
        "  threads              list kernel threads",
        "  echo <text...>       print arguments",
        "  sleep <ms>           yield for N milliseconds",
        "  choco                wisdom from Choco the black cat",
        "  reboot               reboot via the 8042 controller",
        "  ls [dir]             list files (whole tree when no dir)",
        "  cat <path>           print a file",
        "  cd [dir]             change the working directory",
        "  pwd                  print the working directory",
        "  mkdir <path>         create a directory",
        "  rmdir <path>         remove a directory",
        "  rm <path>            remove a file",
        "  mv <src> <dst>       move/rename",
        "  cp <src> <dst>       copy a file",
        "  touch <path>         create an empty file",
        "  tree [dir]           recursive listing",
        "  df                   ram/filesystem summary",
        "  uname                system identification",
        "  date                 show the wall clock (RTC)",
        "  poweroff             power off the machine (ACPI)",
        "  run <path> [args]   run a user program in ring 3",
        "  mpos                 show mouse position and buttons",
        "  mclick <x> <y>       synthetic mouse click (UI testing)",
        "  wintest              synthetic drag test (moves terminal)",
        "  wallpaper [N]        list/select landscape wallpapers",
        "  ifconfig             show MAC/IP/gateway/net stats",
        "  ping <ip> [n]        ICMP echo to IPv4 (try 'ping 10.0.2.2')",
        "  arp                  show ARP table",
        "  dhcp [renew]         DHCP status (renews in background)",
        "  udpstat              UDP sockets and counters",
        "  tcpstat              TCP counters",
        "  httpget <h> [p] [u]  HTTP/1.0 GET (needs TCP + DNS)",
        "  dns <name>           resolve A records via UDP (try 'dns example.com')",
        "  cmd ; cmd            sequence commands",
        "  a && b | a || b      short-circuit on exit status",
        "  cmd > file | >> file  redirect stdout (truncate / append)",
        "  cmd < file           redirect stdin",
        "  a | b                pipe a's output into b (sequential)",
        "  $?                   last exit status (e.g. 'run bin/x; echo $?')",
    };
    con::println("commands:", ACC);
    for (auto *ln : kLines) con::println(ln);
    con::println("line editing: backspace, Ctrl+C cancel, Ctrl+U wipe, Up/Down history", FG);
}

void c_clear(int, char **) {
    con::clear();
    con::banner(TITLE);
}

void c_mem(int, char **) {
    con::kprintf(OK, BG, "[mem] ram total=%u MiB free=%u MiB used=%u MiB (%llu pages)\n",
                 (unsigned)(pmm::total_bytes() / 1048576),
                 (unsigned)(pmm::free_bytes() / 1048576),
                 (unsigned)(pmm::used_bytes() / 1048576), pmm::total_pages());
    con::kprintf(FG, BG, "[mem] heap mapped=%u KiB used=%u B | pml4=0x%lx cr3=0x%lx\n",
                 (unsigned)(heap::mapped_bytes() / 1024),
                 (unsigned)heap::used_bytes(), paging::current_pml4_phys(),
                 paging::read_cr3());
    con::kprintf(FG, BG, "[mem] threads=%llu switches=%llu keys=%llu\n",
                 sched::thread_count(), sched::switches(), kbd::total_keys());
}

void c_uptime(int, char **) {
    con::kprintf(OK, BG, "up %llu s (%llu ticks @ %u Hz)\n",
                 pit::seconds(), pit::ticks(), pit::HZ);
}

void c_threads(int, char **) { sched::dump(); }

// Regression test: a brand-new thread that sits in a tight spin must be
// preempted by the PIT (needs sti in thread_trampoline) and a 1 ms sleep must
// not round to zero ticks (needs sleep_ms rounding up). If either regresses,
// the boot would hang here with only the first worker scheduled.
struct SpinJob { volatile uint64_t ticks; };

void spin_worker(void *arg) {
    (void)arg;
    uint64_t end = pit::ticks() + 20; // ~200 ms at 100 Hz
    volatile uint64_t x = 0;
    while ((int64_t)(pit::ticks() - end) < 0) {
        x++; // tight loop, no yield on purpose
        if ((x & 0xFFFFFFULL) == 0) sched::sleep_ms(1);
    }
    sched::exit();
}

void c_stress(int, char **) {
    con::println("[stress] spawning tight-loop thread (200 ms)...", FG);
    if (!sched::spawn("stress", spin_worker, nullptr)) {
        con::println("[stress] FAIL: spawn refused", ERR);
        return;
    }
    // If preemption and sleep_ms work, control returns here immediately and
    // the shell keeps answering while the spinner runs to completion.
    con::println("[stress] shell responsive while spinner runs - OK", OK);
}

void c_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) con::put_char(' ', FG, BG);
        con::print(argv[i], FG, BG);
    }
    con::put_char('\n', FG, BG);
}

// mkfile <path> <text...> - write text to a file (create or truncate).
// Needed to exercise the writable layer without output redirection.
void c_mkfile(int argc, char **argv) {
    if (argc < 3) {
        con::println("usage: mkfile <path> <text...>", ERR);
        return;
    }
    // Join arguments into the payload, single-spaced.
    char buf[256]{};
    size_t n = 0;
    for (int i = 2; i < argc && n < sizeof(buf) - 2; i++) {
        if (i > 2) buf[n++] = ' ';
        const char *s = argv[i];
        while (*s && n < sizeof(buf) - 2) buf[n++] = *s++;
    }
    buf[n++] = '\n';
    buf[n] = 0;
    const uint8_t *data = nullptr;
    size_t cap = 0;
    long id = fs::writable_open(argv[1], true, true, &data, &cap);
    if (id < 0) {
        con::kprintf(ERR, BG, "mkfile: open failed (%ld)\n", id);
        return;
    }
    size_t next = 0;
    long w = fs::writable_write(id, 0, buf, n, false, &next);
    if (w < 0) {
        con::kprintf(ERR, BG, "mkfile: write failed (%ld)\n", w);
        return;
    }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "mkfile: %ld bytes -> %s\n", w, argv[1]);
}

void c_sleep(int argc, char **argv) {
    uint64_t ms = 1000;
    if (argc > 1 && !util::parse_uint(argv[1], ms)) {
        con::println("usage: sleep <milliseconds>", ERR);
        return;
    }
    con::kprintf(FG, BG, "sleeping %llu ms...\n", ms);
    sched::sleep_ms(ms);
    con::println("awake.", OK);
}

void c_choco(int, char **) {
    static const char *kMsgs[] = {
        "Choco says: all threads are purring in parallel.",
        "Choco says: a clean heap is a happy heap.",
        "Choco says: 100 ticks a second keeps the scheduler fed.",
        "Choco says: have you tried turning it off and on again? (see 'reboot')",
    };
    con::println("  /\\_/\\", FG);
    con::println(" ( o.o )", FG);
    con::println("  > ^ <", FG);
    con::kprintf(ACC, BG, "%s\n", kMsgs[pit::ticks() % 4]);
}

void c_reboot(int, char **) {
    con::println("rebooting...", ACC);
    cpu::outb(0x64, 0xFE); // 8042 reset (works on QEMU/real PCs)
    for (;;) { asm volatile("hlt"); }
}

// Deterministic gesture test: synthesizes pointer motion through
// mouse::inject and checks the terminal window actually moved.
void c_wintest(int, char **) {
    // The desktop boots with the terminal hidden, and a hidden window cannot
    // be grabbed by any hit test - show it first or the gestures test nothing.
    gui::show_term();
    gui::tick();
    int x0 = 0, y0 = 0;
    gui::term_pos(x0, y0);
    // Dismiss the start menu first (an open menu eats the next press,
    // like Win11): click the gap between windows and the taskbar.
    mouse::inject(640, 748, 0);
    gui::tick();
    mouse::inject(640, 748, 1);
    gui::tick();
    mouse::inject(640, 748, 0);
    gui::tick();
    gui::term_pos(x0, y0);
    con::kprintf(FG, BG, "[wintest] term at %d,%d; dragging +100,+80...\n", x0, y0);
    mouse::inject(x0 + 100, y0 + 10, 0);
    gui::tick();
    mouse::inject(x0 + 100, y0 + 10, 1); // press on title bar
    gui::tick();
    for (int i = 1; i <= 4; i++) {
        mouse::inject(x0 + 100 + i * 25, y0 + 10 + i * 20, 1);
        gui::tick();
    }
    mouse::inject(x0 + 200, y0 + 90, 0); // release
    gui::tick();
    int x1 = 0, y1 = 0;
    gui::term_pos(x1, y1);
    bool ok = (x1 == x0 + 100) && (y1 == y0 + 80);
    con::kprintf(ok ? OK : ERR, BG, "[wintest] term now at %d,%d: %s\n",
                 x1, y1, ok ? "PASS (drag works)" : "FAIL");

    // Drag back to the start. The terminal is 1184x696 on a 1280x800 screen,
    // so after +100/+80 it hangs over the right and bottom edges; the resize
    // clamp would then legitimately shorten width/height and the assertions
    // below would be meaningless.
    const int px = x1 + 100, py = y1 + 10;  // grab point on the caption
    mouse::inject(px, py, 0);
    gui::tick();
    mouse::inject(px, py, 1);
    gui::tick();
    for (int i = 1; i <= 4; i++) {
        mouse::inject(px - i * 25, py - i * 20, 1);
        gui::tick();
    }
    mouse::inject(px - 100, py - 80, 0);
    gui::tick();
    gui::term_pos(x1, y1);
    const bool bok = (x1 == x0) && (y1 == y0);
    con::kprintf(bok ? OK : ERR, BG, "[wintest] dragged back to %d,%d: %s\n", x1, y1,
                 bok ? "PASS (reverse drag)" : "FAIL");

    // Resize: grab the LEFT edge and drag it 60px inward. The right/bottom
    // edges are off-screen here (the terminal is 1184x696 on a 1280x800
    // display and the drag clamp lets it hang over the edge), so the pointer
    // could never land on their grab band. Left edge also proves the coupled
    // x/w math: x grows by the same amount the width shrinks.
    int w0 = 0, h0 = 0;
    gui::term_size(w0, h0);
    const int ex = x1, ey = y1 + h0 / 2;
    mouse::inject(ex, ey, 0);
    gui::tick();
    mouse::inject(ex, ey, 1);  // press on the border band
    gui::tick();
    for (int i = 1; i <= 3; i++) {
        mouse::inject(ex + i * 20, ey, 1);
        gui::tick();
    }
    mouse::inject(ex + 60, ey, 0);  // release
    gui::tick();
    int w1 = 0, h1 = 0, xr0 = 0, yr0 = 0;
    gui::term_size(w1, h1);
    gui::term_pos(xr0, yr0);
    const bool rok = (w1 == w0 - 60) && (h1 == h0) && (xr0 == x1 + 60) && (yr0 == y1);
    con::kprintf(rok ? OK : ERR, BG,
                 "[wintest] resize %dx%d@%d,%d -> %dx%d@%d,%d: %s\n", w0, h0, x1, y1, w1, h1,
                 xr0, yr0, rok ? "PASS (edge drag)" : "FAIL");

    // Maximize: double-click the caption (at the post-resize position).
    const int cx = xr0 + 100, cy = yr0 + 10;
    mouse::inject(cx, cy, 1);
    gui::tick();
    mouse::inject(cx, cy, 0);
    gui::tick();
    mouse::inject(cx, cy, 1);  // second press within the double-click window
    gui::tick();
    mouse::inject(cx, cy, 0);
    gui::tick();
    int wm = 0, hm = 0;
    gui::term_size(wm, hm);
    const bool mok = wm > w1 && hm > h1;
    con::kprintf(mok ? OK : ERR, BG, "[wintest] maximize %dx%d -> %dx%d: %s\n", w1, h1, wm, hm,
                 mok ? "PASS (caption double-click)" : "FAIL");

    // Restore: a maximized window sits at 0,0, so its caption is there now.
    mouse::inject(100, 10, 1);
    gui::tick();
    mouse::inject(100, 10, 0);
    gui::tick();
    mouse::inject(100, 10, 1);
    gui::tick();
    mouse::inject(100, 10, 0);
    gui::tick();
    int wr = 0, hr = 0, xr = 0, yr = 0;
    gui::term_size(wr, hr);
    gui::term_pos(xr, yr);
    // Only a real round trip counts: without the maximize above, the geometry
    // would be unchanged and this would pass vacuously.
    const bool rok2 = mok && (wr == w1) && (hr == h1) && (xr == xr0) && (yr == yr0);
    con::kprintf(rok2 ? OK : ERR, BG, "[wintest] restore to %dx%d at %d,%d: %s\n", w1, h1, xr, yr,
                 rok2 ? "PASS (geometry round-trip)" : "FAIL");
}

void c_wallpaper(int argc, char **argv) {
    unsigned n = gui::wallpaper_count();
    if (n == 0) {
        con::println("no photo wallpapers (procedural night). Drop JPGs in icons/ as backgroundN.jpg", FG);
        return;
    }
    if (argc < 2) {
        con::kprintf(FG, BG, "%u wallpaper(s), current %u. Usage: wallpaper <1..%u>\n",
                     n, gui::wallpaper::selected() + 1, n);
        return;
    }
    uint64_t want = 0;
    if (!util::parse_uint(argv[1], want) || want == 0 || want > n) {
        con::kprintf(ERR, BG, "usage: wallpaper <1..%u>\n", n);
        return;
    }
    gui::set_wallpaper((unsigned)want);
    con::kprintf(OK, BG, "wallpaper %llu\n", want);
}

void c_mclick(int argc, char **argv) {
    if (argc < 3) { con::println("usage: mclick <x> <y>", ERR); return; }
    uint64_t x = 0, y = 0;
    if (!util::parse_uint(argv[1], x) || !util::parse_uint(argv[2], y)) {
        con::println("usage: mclick <x> <y>", ERR);
        return;
    }
    mouse::inject((int)x, (int)y, 0);
    gui::tick();
    sched::sleep_ms(150);
    mouse::inject((int)x, (int)y, 1);
    gui::tick();
    sched::sleep_ms(150);
    mouse::inject((int)x, (int)y, 0);
    gui::tick();
    sched::sleep_ms(150);
    con::kprintf(FG, BG, "clicked %llu,%llu\n", x, y);
}

void c_mpos(int, char **) {
    uint8_t raw[3] = {};
    mouse::last_packet(raw);
    con::kprintf(FG, BG, "mouse: present=%u x=%d y=%d buttons=%u packets=%llu last=%x %x %x\n",
                 mouse::present() ? 1u : 0u, mouse::x(), mouse::y(),
                 mouse::buttons(), mouse::packets(), raw[0], raw[1], raw[2]);
    uint8_t h[16]{};
    uint64_t n = 0, total = 0;
    mouse::hist_dump(h, total);
    n = total < 16 ? total : 16;
    con::print("stream bytes=", FG, BG);
    con::kprintf(FG, BG, "%llu: ", total);
    for (uint64_t k = 0; k < n; k++)
        con::kprintf(FG, BG, "%x ", h[k]);
    con::put_char('\n', FG, BG);
}

// --- flat initrd path helpers ("" == root, no leading/trailing slash) ---
char s_cwd[200] = "";

bool same_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

// True when `name` is a direct child of `base`; returns the remainder.
const char *child_of(const char *name, const char *base) {
    size_t bl = util::strlen(base);
    if (!bl) return name;
    if (util::strlen(name) <= bl + 1) return nullptr;
    if (!same_n(name, base, bl) || name[bl] != '/') return nullptr;
    return name + bl + 1;
}

// Resolve `arg` against the shell cwd into a canonical flat path.
bool resolve(const char *arg, char *out, size_t cap) {
    char work[200];
    size_t wl = 0;
    if (!(arg && arg[0] == '/'))
        for (size_t i = 0; s_cwd[i] && i + 1 < sizeof(work); i++) work[wl++] = s_cwd[i];
    work[wl] = 0;
    const char *p = arg;
    while (p && *p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *c = p;
        while (*p && *p != '/') p++;
        size_t cl = (size_t)(p - c);
        if (cl == 1 && c[0] == '.') continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (wl > 0 && work[wl - 1] != '/') wl--;
            if (wl > 0) wl--;
            continue;
        }
        if (wl + cl + 1 >= sizeof(work)) return false;
        if (wl) work[wl++] = '/';
        for (size_t k = 0; k < cl; k++) work[wl++] = c[k];
    }
    work[wl] = 0;
    if (wl + 1 > cap) return false;
    for (size_t k = 0; k <= wl; k++) out[k] = work[k];
    return true;
}

bool dir_exists(const char *path) {
    if (!*path) return true; // root
    for (size_t i = 0; i < fs::count(); i++) {
        const char *n = nullptr;
        size_t sz = 0;
        if (fs::stat(i, &n, &sz) && child_of(n, path)) return true;
    }
    return false;
}

// Unique sub-directory names directly under `base`.
size_t collect_dirs(const char *base, char dirs[][64], size_t maxd) {
    size_t nd = 0;
    for (size_t i = 0; i < fs::count(); i++) {
        const char *n = nullptr;
        size_t sz = 0;
        if (!fs::stat(i, &n, &sz)) continue;
        const char *r = child_of(n, base);
        if (!r) continue;
        const char *sl = r;
        while (*sl && *sl != '/') sl++;
        if (!*sl) continue; // a file
        size_t len = (size_t)(sl - r);
        if (len >= 64 || nd >= maxd) continue;
        bool dup = false;
        for (size_t d = 0; d < nd; d++)
            if (util::strlen(dirs[d]) == len && same_n(dirs[d], r, len)) { dup = true; break; }
        if (dup) continue;
        for (size_t k = 0; k < len; k++) dirs[nd][k] = r[k];
        dirs[nd][len] = 0;
        nd++;
    }
    return nd;
}

void list_dir(const char *base) {
    char dirs[32][64];
    size_t nd = collect_dirs(base, dirs, 32);
    for (size_t d = 0; d < nd; d++) con::kprintf(ACC, BG, "  %s/\n", dirs[d]);
    for (size_t i = 0; i < fs::count(); i++) {
        const char *n = nullptr;
        size_t sz = 0;
        if (!fs::stat(i, &n, &sz)) continue;
        const char *r = child_of(n, base);
        if (!r) continue;
        const char *sl = r;
        while (*sl && *sl != '/') sl++;
        if (*sl) continue; // subdir, listed above
        con::kprintf(FG, BG, "  %s (%u B)\n", r, (unsigned)sz);
    }
}

void c_ls(int argc, char **argv) {
    if (!fs::count()) { con::println("(initrd empty: no boot module found)", ERR); return; }
    if (argc < 2) { // no argument: flat listing of the whole tree (legacy)
        for (size_t i = 0; i < fs::count(); i++) {
            const char *n = nullptr;
            size_t sz = 0;
            if (fs::stat(i, &n, &sz))
                con::kprintf(FG, BG, "  %s (%u B)\n", n, (unsigned)sz);
        }
        return;
    }
    char p[200];
    if (!resolve(argv[1], p, sizeof(p))) { con::println("ls: bad path", ERR); return; }
    list_dir(p);
}

void c_cat(int argc, char **argv) {
    if (argc < 2) { con::println("usage: cat <path>", ERR); return; }
    const void *d = nullptr;
    size_t sz = 0;
    if (!fs::read(argv[1], &d, &sz)) {
        con::kprintf(ERR, BG, "cat: '%s' not found (try 'ls')\n", argv[1]);
        return;
    }
    const char *s = (const char *)d;
    for (size_t i = 0; i < sz; i++) con::put_char(s[i], FG, BG);
    if (sz && s[sz - 1] != '\n') con::put_char('\n', FG, BG);
}

void c_pwd(int, char **) { con::kprintf(FG, BG, "/%s\n", s_cwd); }

void c_cd(int argc, char **argv) {
    if (argc < 2) { s_cwd[0] = 0; return; } // cd with no arg -> root
    char p[200];
    if (!resolve(argv[1], p, sizeof(p))) { con::println("cd: path too long", ERR); return; }
    if (!dir_exists(p)) {
        con::kprintf(ERR, BG, "cd: no such directory: %s\n", argv[1]);
        return;
    }
    size_t n = 0;
    while (p[n] && n + 1 < sizeof(s_cwd)) s_cwd[n] = p[n], n++;
    s_cwd[n] = 0;
}

void c_mkdir(int argc, char **argv) {
    char p[200];
    if (argc < 2 || !resolve(argv[1], p, sizeof(p)) || !*p) {
        con::println("usage: mkdir <path>", ERR);
        return;
    }
    if (!fs::writable_mkdir(p)) { con::kprintf(ERR, BG, "mkdir: failed: %s\n", argv[1]); return; }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "mkdir: %s\n", p);
}

void c_rmdir(int argc, char **argv) {
    char p[200];
    if (argc < 2 || !resolve(argv[1], p, sizeof(p)) || !*p) {
        con::println("usage: rmdir <path>", ERR);
        return;
    }
    if (!fs::writable_rmdir(p)) { con::kprintf(ERR, BG, "rmdir: failed: %s\n", argv[1]); return; }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "rmdir: %s\n", p);
}

void c_rm(int argc, char **argv) {
    char p[200];
    if (argc < 2 || !resolve(argv[1], p, sizeof(p)) || !*p) {
        con::println("usage: rm <path>", ERR);
        return;
    }
    if (!fs::writable_unlink(p)) { con::kprintf(ERR, BG, "rm: failed: %s\n", argv[1]); return; }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "rm: %s\n", p);
}

void c_mv(int argc, char **argv) {
    char a[200], b[200];
    if (argc < 3 || !resolve(argv[1], a, sizeof(a)) || !resolve(argv[2], b, sizeof(b)) || !*a ||
        !*b) {
        con::println("usage: mv <src> <dst>", ERR);
        return;
    }
    if (!fs::writable_rename(a, b)) {
        con::kprintf(ERR, BG, "mv: failed: %s -> %s\n", a, b);
        return;
    }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "mv: %s -> %s\n", a, b);
}

void c_cp(int argc, char **argv) {
    char a[200], b[200];
    if (argc < 3 || !resolve(argv[1], a, sizeof(a)) || !resolve(argv[2], b, sizeof(b)) || !*a ||
        !*b) {
        con::println("usage: cp <src> <dst>", ERR);
        return;
    }
    const void *src = nullptr;
    size_t sz = 0;
    if (!fs::read(a, &src, &sz)) { con::kprintf(ERR, BG, "cp: no such file: %s\n", a); return; }
    const uint8_t *dst = nullptr;
    size_t cap = 0;
    long id = fs::writable_open(b, true, true, &dst, &cap);
    if (id < 0) { con::kprintf(ERR, BG, "cp: cannot create %s (%ld)\n", b, id); return; }
    size_t next = 0;
    long w = fs::writable_write(id, 0, src, sz, false, &next);
    if (w < 0) { con::kprintf(ERR, BG, "cp: write failed (%ld)\n", w); return; }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "cp: %s -> %s (%ld bytes)\n", a, b, w);
}

void c_touch(int argc, char **argv) {
    char p[200];
    if (argc < 2 || !resolve(argv[1], p, sizeof(p)) || !*p) {
        con::println("usage: touch <path>", ERR);
        return;
    }
    if (fs::read(p, nullptr, nullptr)) { con::kprintf(FG, BG, "touch: %s (exists)\n", p); return; }
    const uint8_t *d = nullptr;
    size_t cap = 0;
    long id = fs::writable_open(p, true, false, &d, &cap);
    if (id < 0) { con::kprintf(ERR, BG, "touch: cannot create %s (%ld)\n", p, id); return; }
    fs::disk_persistence_flush();
    con::kprintf(OK, BG, "touch: %s\n", p);
}

void tree_rec(const char *base, int depth) {
    if (depth > 8) return;
    char dirs[32][64];
    size_t nd = collect_dirs(base, dirs, 32);
    for (size_t d = 0; d < nd; d++) {
        for (int i = 0; i < depth; i++) con::print("  ", FG, BG);
        con::kprintf(ACC, BG, "%s/\n", dirs[d]);
        char child[200];
        size_t n = 0;
        for (const char *s = base; *s && n + 1 < sizeof(child); s++) child[n++] = *s;
        if (n) child[n++] = '/';
        for (const char *s = dirs[d]; *s && n + 1 < sizeof(child); s++) child[n++] = *s;
        child[n] = 0;
        tree_rec(child, depth + 1);
    }
    for (size_t i = 0; i < fs::count(); i++) {
        const char *nm = nullptr;
        size_t sz = 0;
        if (!fs::stat(i, &nm, &sz)) continue;
        const char *r = child_of(nm, base);
        if (!r) continue;
        const char *sl = r;
        while (*sl && *sl != '/') sl++;
        if (*sl) continue; // subdir, handled above
        for (int k = 0; k < depth; k++) con::print("  ", FG, BG);
        con::kprintf(FG, BG, "%s\n", r);
    }
}

void c_tree(int argc, char **argv) {
    char p[200];
    if (argc < 2) {
        size_t i = 0;
        for (; s_cwd[i] && i + 1 < sizeof(p); i++) p[i] = s_cwd[i];
        p[i] = 0;
    } else if (!resolve(argv[1], p, sizeof(p))) {
        con::println("tree: bad path", ERR);
        return;
    }
    con::kprintf(ACC, BG, "/%s\n", p);
    tree_rec(p, 1);
}

void c_df(int, char **) {
    const uint64_t total = pmm::total_bytes() / 1048576, freeb = pmm::free_bytes() / 1048576;
    con::kprintf(FG, BG, "ram    %llu MiB free / %llu MiB total\n", freeb, total);
    con::kprintf(FG, BG, "heap   %u KiB mapped, %u B used\n",
                 (unsigned)(heap::mapped_bytes() / 1024), (unsigned)heap::used_bytes());
    con::kprintf(FG, BG, "initrd %u entries\n", (unsigned)fs::count());
    con::kprintf(FG, BG, "disk   %s%s\n", fs::fat32::mounted() ? "mounted" : "absent",
                 (fs::fat32::mounted() && fs::fat32::writable()) ? " (rw)" : " (ro)");
}

void c_uname(int, char **) {
    con::println("ChocoOS 0.7 x86_64 (Limine) - ring-3 + Linux ABI");
}

void c_date(int, char **) {
    const uint64_t e = rtc::now();
    const int64_t days = (int64_t)(e / 86400);
    unsigned rem = (unsigned)(e % 86400);
    int64_t z = days + 719468; // civil_from_days (Howard Hinnant)
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mo = mp + (mp < 10 ? 3 : (unsigned)-9);
    y += (mo <= 2);

    char b[40];
    uint64_t p = 0;
    auto n4 = [&](int v) {
        b[p++] = (char)('0' + (v / 1000) % 10);
        b[p++] = (char)('0' + (v / 100) % 10);
        b[p++] = (char)('0' + (v / 10) % 10);
        b[p++] = (char)('0' + v % 10);
    };
    auto n2 = [&](unsigned v) {
        b[p++] = (char)('0' + (v / 10) % 10);
        b[p++] = (char)('0' + v % 10);
    };
    n4((int)y);
    b[p++] = '-'; n2(mo);
    b[p++] = '-'; n2(d);
    b[p++] = ' '; n2(rem / 3600);
    b[p++] = ':'; n2((rem / 60) % 60);
    b[p++] = ':'; n2(rem % 60);
    b[p] = 0;
    con::kprintf(rtc::available() ? FG : ERR, BG, "date: %s UTC%s\n", b,
                 rtc::available() ? "" : " (no RTC, uptime-based)");
}

void c_poweroff(int, char **) {
    con::println("powering off...", ACC);
    gui::poweroff(); // never returns
}

void c_run(int argc, char **argv) {
    if (argc < 2) { con::println("usage: run <path> [args...]  (try 'run bin/hello foo')", ERR); return; }
    if (s_bg) { // '&': detached job; the ambient loop (gui::tick) reaps it
        uint32_t pid = 0;
        if (proc::spawn_job(argv[1], argc - 2, &argv[2], &pid) == 0)
            con::kprintf(ACC, BG, "[bg] %s pid %u\n", argv[1], pid);
        else con::println("run: cannot start background job", ERR);
        return;
    }
    if (s_has_out || s_has_in)
        proc::run_args_redir(argv[1], argc - 2, &argv[2], s_has_out ? s_rout : nullptr,
                             s_rapp, s_has_in ? s_rin : nullptr);
    else
        proc::run_args(argv[1], argc - 2, &argv[2]);
}

void c_ifconfig(int, char **) {
    if (!net::ready()) { con::println("net: no NIC (boot without -nic?)", ERR); return; }
    char ip[16], gw[16], mk[16], dn[16], mc[18];
    net::fmt_ipv4(net::ip(), ip);
    net::fmt_ipv4(net::gw(), gw);
    net::fmt_ipv4(net::mask(), mk);
    net::fmt_ipv4(net::dns_server(), dn);
    net::fmt_mac(net::mac(), mc);
    const auto &st = net::stats();
    con::kprintf(FG, BG, "mac %s link %s\n", mc, net::link_up() ? "up" : "down");
    con::kprintf(FG, BG, "ip %s mask %s gw %s dns %s src %s dhcp %s\n", ip, mk, gw, dn,
                 net::ip_source_str(), net::dhcp::state_str());
    con::kprintf(FG, BG, "rx %llu tx %llu drop %llu | arp rx %llu tx %llu | ip rx %llu tx %llu | icmp rx %llu tx %llu reply %llu\n",
                 st.rx, st.tx, st.dropped, st.arp_rx, st.arp_tx,
                 st.ip_rx, st.ip_tx, st.icmp_rx, st.icmp_tx, st.icmp_reply);
}

void c_ping(int argc, char **argv) {
    if (argc < 2) { con::println("usage: ping <ipv4> [count]", ERR); return; }
    if (!net::ready()) { con::println("net: no NIC", ERR); return; }
    uint32_t dst = 0;
    if (!net::parse_ipv4(argv[1], dst)) {
        con::kprintf(ERR, BG, "ping: bad IPv4 '%s'\n", argv[1]);
        return;
    }
    uint64_t n = 4;
    if (argc > 2 && !util::parse_uint(argv[2], n)) {
        con::println("usage: ping <ipv4> [count]", ERR);
        return;
    }
    if (n == 0 || n > 100) n = 4;
    char tmp[16];
    net::fmt_ipv4(dst, tmp);
    con::kprintf(FG, BG, "[ping] %s x%llu...\n", tmp, n);
    int got = net::ping(dst, (int)n, 2000);
    con::kprintf(got > 0 ? OK : ERR, BG, "[ping] %d/%llu replies\n", got, n);
}

void c_arp(int argc, char **argv) {
    if (argc > 1 && !util::strcmp(argv[1], "flush")) {
        net::arp_flush();
        con::println("arp: table flushed", FG);
        return;
    }
    if (!net::ready()) { con::println("net: no NIC", ERR); return; }
    int n = net::arp_count();
    con::kprintf(FG, BG, "arp: %d entries\n", n);
    for (int i = 0; i < n; i++) {
        uint32_t ip = 0;
        uint8_t m[6]{};
        if (!net::arp_get(i, &ip, m)) break;
        char ips[16], mcs[18];
        net::fmt_ipv4(ip, ips);
        net::fmt_mac(m, mcs);
        con::kprintf(FG, BG, "  %s -> %s\n", ips, mcs);
    }
}

void c_dns(int argc, char **argv) {
    if (argc < 2) {
        con::println("usage: dns <name>  (try 'dns example.com')", ERR);
        return;
    }
    if (!net::ready()) {
        con::println("net: no NIC", ERR);
        return;
    }
    con::kprintf(FG, BG, "[dns] resolving '%s' ...\n", argv[1]);
    uint32_t ips[4]{};
    int rc = net::dns::resolve(argv[1], ips, 4, 6000);
    if (rc > 0) {
        for (int i = 0; i < rc; i++) {
            char tmp[16];
            net::fmt_ipv4(ips[i], tmp);
            con::kprintf(OK, BG, "[dns] %s -> %s\n", argv[1], tmp);
        }
    } else if (rc == 0) {
        con::kprintf(FG, BG, "[dns] %s: no A records\n", argv[1]);
    } else if (rc == -3) {
        con::kprintf(ERR, BG, "[dns] %s: NXDOMAIN\n", argv[1]);
    } else if (rc == -2) {
        con::kprintf(ERR, BG, "[dns] bad name '%s'\n", argv[1]);
    } else if (rc == -4) {
        con::kprintf(ERR, BG, "[dns] %s: server failure\n", argv[1]);
    } else if (rc == -5) {
        con::kprintf(ERR, BG, "[dns] %s: malformed response\n", argv[1]);
    } else {
        con::kprintf(ERR, BG, "[dns] %s: timeout\n", argv[1]);
    }
}

void c_dhcp(int argc, char **argv) {
    if (argc > 1 && !util::strcmp(argv[1], "renew")) {
        if (net::dhcp::renew())
            con::println("dhcp: renewal started (watch serial/log)", FG);
        else
            con::println("dhcp: a run is already active", ERR);
        return;
    }
    char sip[16] = "-";
    if (net::dhcp::server()) net::fmt_ipv4(net::dhcp::server(), sip);
    char ips[16];
    net::fmt_ipv4(net::ip(), ips);
    con::kprintf(FG, BG, "dhcp: %s ip %s src %s server %s lease %u s\n",
                 net::dhcp::state_str(), ips, net::ip_source_str(), sip,
                 net::dhcp::lease_secs());
}

void c_udpstat(int, char **) {
    const auto &st = net::udp::stats();
    con::kprintf(FG, BG, "udp: socks %d rx %llu tx %llu drop %llu csum_err %llu\n",
                 net::udp::socket_count(), st.rx, st.tx, st.dropped, st.csum_err);
}

void c_tcpstat(int, char **) {
    const auto &st = net::tcp::stats();
    const auto &n = net::stats();
    con::kprintf(FG, BG, "tcp: rx %llu tx %llu retrans %llu rst %llu drop %llu | ip rx %llu tx %llu\n",
                 st.rx_segs, st.tx_segs, st.retrans, st.rst_rx, st.dropped, n.ip_rx,
                 n.ip_tx);
}

void c_httpget(int argc, char **argv) {
    // Kernel-side HTTP/1.0 GET over the internal TCP engine (no syscalls:
    // the syscall/socket layer in net-5 will reuse tcp::connect/send/recv).
    // With a 4th argument the response body is written to that path too,
    // which lets us test the full chain: TCP fetch + FAT32 write + reboot check.
    if (argc < 2) {
        con::println("usage: httpget <host> [port] [path] [outfile]  (try 'httpget 10.0.2.2 18080 /')",
                     ERR);
        return;
    }
    const char *host = argv[1];
    uint64_t port = 80;
    if (argc > 2 && !util::parse_uint(argv[2], port)) {
        con::println("usage: httpget <host> [port] [path] [outfile]", ERR);
        return;
    }
    const char *path = (argc > 3) ? argv[3] : "/";
    if (port == 0 || port > 65535) {
        con::println("httpget: bad port", ERR);
        return;
    }
    const char *outfile = (argc > 4) ? argv[4] : nullptr;
    uint32_t ip = 0;
    if (!net::parse_ipv4(host, ip)) {
        uint32_t ips[2]{};
        int rc = net::dns::resolve(host, ips, 2, 6000);
        if (rc <= 0) {
            con::kprintf(ERR, BG, "httpget: cannot resolve '%s' (%d)\n", host, rc);
            return;
        }
        ip = ips[0];
    }
    char ips[16];
    net::fmt_ipv4(ip, ips);
    con::kprintf(FG, BG, "[http] GET http://%s:%llu%s (%s)...\n", host, port, path, ips);
    net::tcp::Handle h = net::tcp::connect(ip, (uint16_t)port, 10000);
    if (h < 0) {
        con::kprintf(ERR, BG, "[http] connect %s:%llu failed\n", ips, port);
        return;
    }
    char req[256];
    size_t rl = 0;
    const char *p1 = "GET ";
    const char *p2 = " HTTP/1.0\r\nHost: ";
    const char *p3 = "\r\nConnection: close\r\n\r\n";
    auto put = [&](const char *s) {
        while (*s && rl < sizeof(req) - 1) req[rl++] = *s++;
    };
    put(p1);
    put(path);
    put(p2);
    put(host);
    put(p3);
    if (net::tcp::send(h, req, (uint16_t)rl) < 0) {
        con::println("[http] send failed", ERR);
        net::tcp::close(h);
        return;
    }
    uint32_t total = 0;
    bool got_status = false;
    // Optional file receive: buffer the body and flush at the end, so a
    // download can land on the persistent volume for the next boot to read.
    uint8_t *file_body = nullptr;
    uint32_t file_size = 0;
    uint32_t file_cap = 0;
    for (;;) {
        static uint8_t buf[1024];
        int n = net::tcp::recv(h, buf, sizeof(buf), 3000);
        if (n < 0) {
            con::kprintf(ERR, BG, "[http] recv error after %u bytes\n", total);
            break;
        }
        if (n == 0) break;  // EOF (server FIN)
        total += (uint32_t)n;
        if (!got_status) {
            got_status = true;
            con::print("[http] status: ", FG, BG);
            for (int i = 0; i < n && buf[i] != '\n' && i < 80; i++)
                con::put_char((char)buf[i], FG, BG);
            con::put_char('\n', FG, BG);
        }
        for (int i = 0; i < n; i++) con::put_char((char)buf[i], FG, BG);
        if (outfile) {
            if (file_size + (uint32_t)n > file_cap) {
                uint32_t grown = file_cap ? file_cap * 2 : 4096;
                if (grown > 256 * 1024) grown = 256 * 1024; // cap large downloads
                uint8_t *nb = (uint8_t *)heap::realloc(file_body, grown);
                if (!nb) { con::println("[http] out of memory", ERR); break; }
                file_body = nb;
                file_cap = grown;
            }
            if (file_size + (uint32_t)n <= file_cap) {
                util::memcpy(file_body + file_size, buf, (size_t)n);
                file_size += (uint32_t)n;
            }
        }
        if (total > 8192 && !outfile) {
            con::println("[http] body cap reached, closing", FG);
            break;
        }
        if (file_size > 256 * 1024) {
            con::println("[http] file too large, truncated", ERR);
            break;
        }
    }
    con::kprintf(total ? OK : ERR, BG, "[http] done: %u body bytes\n", total);
    net::tcp::close(h);

    if (outfile && file_body && file_size) {
        const uint8_t *dst = nullptr;
        size_t cap = 0;
        long id = fs::writable_open(outfile, true, true, &dst, &cap);
        if (id < 0) {
            con::kprintf(ERR, BG, "[http] cannot open '%s' (%ld)\n", outfile, id);
        } else {
            size_t wro = 0;
            long w = fs::writable_write(id, 0, file_body, file_size, false, &wro);
            if (w >= 0) {
                fs::disk_persistence_flush();
                con::kprintf(OK, BG, "[http] saved %ld bytes -> %s\n", w, outfile);
            } else {
                con::kprintf(ERR, BG, "[http] write failed (%ld)\n", w);
            }
        }
        heap::free(file_body);
    } else if (file_body) {
        heap::free(file_body);
    }
}

void c_pci(int, char **) {
    int n = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint16_t ven =
                    pci::read_config16((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                if (ven == 0xFFFF) continue;
                uint16_t dev =
                    pci::read_config16((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x02);
                uint16_t cmd =
                    pci::read_config16((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04);
                con::kprintf(FG, BG, "pci %x:%x.%d ven %x dev %x cmd %x\n", bus, slot, func,
                             ven, dev, cmd);
                if (++n > 32) {
                    con::println("(truncated)", FG);
                    return;
                }
            }
        }
    }
    con::kprintf(FG, BG, "pci: %d device(s)\n", n);
}

struct Cmd { const char *name; const char *help; CmdFn fn; };
const Cmd kCmds[] = {
    {"help", "show this list", c_help},
    {"clear", "clear the screen", c_clear},
    {"mem", "memory summary", c_mem},
    {"uptime", "uptime", c_uptime},
    {"threads", "list threads", c_threads},
    {"stress", "spawn tight-loop thread: preemption regression", c_stress},
    {"echo", "print arguments", c_echo},
    {"mkfile", "mkfile <path> <text...> - write text to file", c_mkfile},
    {"sleep", "sleep N ms", c_sleep},
    {"choco", "black-cat wisdom", c_choco},
    {"reboot", "reboot the machine", c_reboot},
    {"ls", "list initrd files", c_ls},
    {"cat", "print a file", c_cat},
    {"cd", "change working directory", c_cd},
    {"pwd", "print working directory", c_pwd},
    {"mkdir", "create a directory", c_mkdir},
    {"rmdir", "remove a directory", c_rmdir},
    {"rm", "remove a file", c_rm},
    {"mv", "move/rename a file", c_mv},
    {"cp", "copy a file", c_cp},
    {"touch", "create an empty file", c_touch},
    {"tree", "recursive listing", c_tree},
    {"df", "ram/filesystem summary", c_df},
    {"uname", "system identification", c_uname},
    {"date", "show the wall clock", c_date},
    {"poweroff", "power off the machine", c_poweroff},
    {"run", "run a user program", c_run},
    {"mpos", "mouse position", c_mpos},
    {"mclick", "synthetic mouse click", c_mclick},
    {"wintest", "synthetic drag test", c_wintest},
    {"wallpaper", "select wallpaper", c_wallpaper},
    {"ifconfig", "show net config", c_ifconfig},
    {"ping", "ping an IPv4", c_ping},
    {"arp", "show ARP table", c_arp},
    {"dns", "resolve a name", c_dns},
    {"dhcp", "DHCP status/renew", c_dhcp},
    {"udpstat", "UDP stats", c_udpstat},
    {"tcpstat", "TCP stats", c_tcpstat},
    {"httpget", "HTTP GET via TCP", c_httpget},
    {"pci", "list PCI devices", c_pci},
};

// Try to run a program from initrd: handles "./x" and bare "x" (tries bin/x).
// On success runs it (honouring the current redirect state) and stores the
// child exit code in *code_out.
bool try_run_program(int argc, char **argv, long *code_out) {
    const char *name = argv[0];
    char path[LINE_MAX];
    const void *data = nullptr;
    size_t size = 0;
    const char *outp = s_has_out ? s_rout : nullptr;
    const char *inp = s_has_in ? s_rin : nullptr;
    auto run = [&](const char *p) -> bool {
        if (!fs::read(p, &data, &size)) return false;
        if (s_bg) { // '&': detached job, reaped by the ambient loop
            uint32_t pid = 0;
            if (proc::spawn_job(p, argc - 1, argv + 1, &pid) != 0) return false;
            con::kprintf(ACC, BG, "[bg] %s pid %u\n", p, pid);
            *code_out = 0;
            return true;
        }
        *code_out = proc::run_args_redir(p, argc - 1, argv + 1, outp, s_rapp, inp);
        return true;
    };

    // Case 1: explicit path with "./" prefix -> strip "./"
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
        if (run(name)) return true; // e.g. "./bin/hello" -> "bin/hello"
        size_t i = 0;               // ... otherwise try bin/<name>
        const char *prefix = "bin/";
        while (*prefix && i < LINE_MAX - 1) path[i++] = *prefix++;
        const char *p = name;
        while (*p && i < LINE_MAX - 1) path[i++] = *p++;
        path[i] = 0;
        return run(path);
    }

    // Case 2: explicit path containing '/' (e.g. bin/hello) -> run as-is
    for (const char *p = name; *p; p++)
        if (*p == '/') return run(name);

    // Case 3: bare name (e.g. "micro") -> try bin/<name>
    size_t i = 0;
    const char *prefix = "bin/";
    while (*prefix && i < LINE_MAX - 1) path[i++] = *prefix++;
    const char *p = name;
    while (*p && i < LINE_MAX - 1) path[i++] = *p++;
    path[i] = 0;
    return run(path);
}

// --- Tier 2: tokenizer, quoting, ';' / '&&' / '||' and redirection ---

constexpr int T_WORD = 0, T_SEMI = 1, T_AND = 2, T_OR = 3;
constexpr int T_BG = 8; // lone '&': background the job (T_GT..T_PIPE use 4..7)
constexpr int T_GT = 4, T_GTGT = 5, T_LT = 6, T_PIPE = 7;

struct Tok {
    char *s;
    int op;
};

size_t emit_status(char *d) { // expands $? into d; returns its length
    char t[16];
    int v = s_last_status < 0 ? 0 : s_last_status;
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (int i = 0; i < n; i++) d[i] = t[n - 1 - i];
    return (size_t)n;
}

// Expands $? in a word at execution time (empty the status is that of the
// previous command). Returns the word itself when it has no $?.
char *expand_word(char *w) {
    bool has = false;
    for (char *p = w; p[0]; p++)
        if (p[0] == '$' && p[1] == '?') { has = true; break; }
    if (!has) return w;
    static char pool[8][256];
    static int idx = 0;
    char *b = pool[idx];
    idx = (idx + 1) & 7;
    size_t o = 0;
    for (char *p = w; p[0] && o + 4 < 256;) {
        if (p[0] == '$' && p[1] == '?') { o += emit_status(b + o); p += 2; }
        else b[o++] = *p++;
    }
    b[o] = 0;
    return b;
}

size_t tokenize(char *s, Tok *out, size_t max) {
    // Words are copied into a scratch area (not the input line): in-place
    // NUL-termination would clobber the delimiter the scanner still needs.
    static char wbuf[1024];
    size_t wn = 0;
    size_t n = 0;
    char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char c = *p;
        if (c == ';') { out[n].s = p; out[n++].op = T_SEMI; p++; continue; }
        if (c == '&') {
            if (p[1] == '&') { out[n].s = p; out[n++].op = T_AND; p += 2; }
            else { out[n].s = p; out[n++].op = T_BG; p++; } // lone '&' -> background
            continue;
        }
        if (c == '|') {
            if (p[1] == '|') { out[n].s = p; out[n++].op = T_OR; p += 2; }
            else { out[n].s = p; out[n++].op = T_PIPE; p++; }
            continue;
        }
        if (c == '>') {
            if (p[1] == '>') { out[n].s = p; out[n++].op = T_GTGT; p += 2; }
            else { out[n].s = p; out[n++].op = T_GT; p++; }
            continue;
        }
        if (c == '<') { out[n].s = p; out[n++].op = T_LT; p++; continue; }
        // word: supports '..' literal and ".." (with \escapes); $? stays literal
        char *w = &wbuf[wn];
        bool sq = false, dq = false;
        while (*p) {
            char ch = *p;
            if (sq) {
                if (ch == '\'') { sq = false; p++; continue; }
                if (wn + 1 < sizeof(wbuf)) wbuf[wn++] = ch;
                p++;
                continue;
            }
            if (dq) {
                if (ch == '"') { dq = false; p++; continue; }
                if (ch == '\\' && p[1]) { if (wn + 1 < sizeof(wbuf)) wbuf[wn++] = p[1]; p += 2; continue; }
                if (wn + 1 < sizeof(wbuf)) wbuf[wn++] = ch;
                p++;
                continue;
            }
            if (ch == '\'') { sq = true; p++; continue; }
            if (ch == '"') { dq = true; p++; continue; }
            if (ch == '\\' && p[1]) { if (wn + 1 < sizeof(wbuf)) wbuf[wn++] = p[1]; p += 2; continue; }
            if (ch == ' ' || ch == '\t' || ch == ';' || ch == '&' || ch == '|' || ch == '>' ||
                ch == '<')
                break;
            if (wn + 1 < sizeof(wbuf)) wbuf[wn++] = ch;
            p++;
        }
        if (wn + 1 < sizeof(wbuf)) wbuf[wn++] = 0; // terminator
        out[n].s = w;
        out[n++].op = T_WORD;
    }
    return n;
}

bool write_all(const char *path, const char *data, size_t len, bool append) {
    if (!path || !*path) return false;
    const uint8_t *d = nullptr; // path is already resolved by the caller
    size_t size = 0;
    long id = fs::writable_open(path, true, !append, &d, &size);
    if (id < 0) return false;
    size_t off = append ? size : 0;
    size_t next = off;
    long w = fs::writable_write(id, off, data, len, false, &next);
    if (w < 0) return false;
    fs::disk_persistence_flush();
    return true;
}

// Dispatch one command: builtins capture their console output when '>' is
// active; 'run' and programs use run_args_redir instead (they spawn a job).
int run_one(int argc, char **argv, bool background) {
    s_bg = background; // consulted by try_run_program / c_run to detach the job
    for (auto &c : kCmds) {
        if (!util::strcmp(argv[0], c.name)) {
            if (s_has_out && util::strcmp(argv[0], "run") != 0) {
                constexpr size_t CAP = 4096;
                char *buf = (char *)heap::alloc(CAP);
                if (!buf) { c.fn(argc, argv); return 0; }
                console::set_capture(buf, CAP);
                c.fn(argc, argv);
                size_t len = console::capture_end();
                if (!write_all(s_rout, buf, len, s_rapp))
                    con::kprintf(ERR, BG, "redirection: cannot write %s\n", s_rout);
                heap::free(buf);
            } else {
                c.fn(argc, argv);
            }
            return 0;
        }
    }
    long code = 0;
    if (try_run_program(argc, argv, &code)) return (int)code;
    con::kprintf(ERR, BG, "unknown command '%s' (try 'help' or 'ls')\n", argv[0]);
    return 127;
}

void copy_str(char *dst, const char *src, size_t cap) {
    size_t n = 0;
    while (src[n] && n + 1 < cap) dst[n] = src[n], n++;
    dst[n] = 0;
}

void make_temp(char *dst, int seq) {
    size_t n = 0;
    dst[n++] = '.';
    dst[n++] = 'p';
    char t[12];
    int m = 0;
    if (seq == 0) t[m++] = '0';
    while (seq) { t[m++] = (char)('0' + seq % 10); seq /= 10; }
    for (int k = 0; k < m; k++) dst[n++] = t[m - 1 - k];
    dst[n] = 0;
}

// Runs the pipeline starting at i (segments delimited by '|') up to the next
// job connector (';' / '&&' / '||'). Pipe segments are chained through scratch
// files, so piping is *sequential*: the producer must finish before the
// consumer starts. That covers finite streams and needs no multi-process
// change to the single-foreground process model. Returns the last segment's
// exit status.
int run_pipeline(Tok *toks, size_t ntok, size_t &i, bool run, int &seq) {
    char inpath[200];
    inpath[0] = 0;
    bool have_in = false;
    int status = s_last_status;
    char temps[8][32];
    int ntemp = 0;
    for (;;) {
        char *argv[ARGS_MAX];
        int argc = 0;
        char rout[200];
        bool has_out = false, app = false;
        char rin[200];
        bool has_in = false;
        while (i < ntok && toks[i].op != T_PIPE && toks[i].op != T_SEMI && toks[i].op != T_AND &&
               toks[i].op != T_OR && toks[i].op != T_BG) {
            if (toks[i].op == T_WORD) {
                if (argc < (int)ARGS_MAX) argv[argc++] = expand_word(toks[i].s);
                i++;
            } else if (toks[i].op == T_GT || toks[i].op == T_GTGT || toks[i].op == T_LT) {
                const int op = toks[i].op;
                i++;
                if (i < ntok && toks[i].op == T_WORD) {
                    if (op == T_LT) { resolve(toks[i].s, rin, sizeof(rin)); has_in = true; }
                    else { resolve(toks[i].s, rout, sizeof(rout)); has_out = true; app = (op == T_GTGT); }
                    i++;
                } else {
                    con::println("syntax error: missing redirection target", ERR);
                }
            } else {
                i++;
            }
        }
        const bool more = (i < ntok && toks[i].op == T_PIPE);
        const bool bg = (i < ntok && toks[i].op == T_BG);
        char outres[200];
        outres[0] = 0;
        if (more) {
            char tname[32];
            make_temp(tname, ++seq);
            resolve(tname, outres, sizeof(outres)); // resolved once; reused as-is
            if (ntemp < 8) copy_str(temps[ntemp++], outres, sizeof(temps[0]));
        }
        if (run) {
            s_has_out = false; s_rapp = false; s_has_in = false;
            if (more) { copy_str(s_rout, outres, sizeof(s_rout)); s_has_out = true; s_rapp = false; }
            else if (has_out) { copy_str(s_rout, rout, sizeof(s_rout)); s_has_out = true; s_rapp = app; }
            if (has_in) { copy_str(s_rin, rin, sizeof(s_rin)); s_has_in = true; }
            else if (have_in) { copy_str(s_rin, inpath, sizeof(s_rin)); s_has_in = true; }
            if (argc > 0) status = run_one(argc, argv, bg);
            s_last_status = status;
        }
        if (more) {
            copy_str(inpath, outres, sizeof(inpath));
            have_in = true;
            i++; // consume '|'
            continue;
        }
        break;
    }
    if (run)
        for (int t = 0; t < ntemp; t++) fs::writable_unlink(temps[t]);
    return status;
}

void exec(char *line) {
    static Tok toks[64];
    size_t ntok = tokenize(line, toks, 64);
    if (!ntok) return;
    size_t i = 0;
    int pending = T_SEMI; // first job runs unconditionally
    static int pipe_seq = 0;
    while (i < ntok) {
        const bool do_run = (pending == T_SEMI) || (pending == T_AND && s_last_status == 0) ||
                            (pending == T_OR && s_last_status != 0);
        s_last_status = run_pipeline(toks, ntok, i, do_run, pipe_seq);
        if (i < ntok) {
            pending = toks[i].op; // T_SEMI / T_AND / T_OR / T_BG
            if (pending == T_BG) pending = T_SEMI; // the background job ends the item
            i++;
        } else {
            break;
        }
    }
}

void handle_key(char c) {
    if (c == kbd::KEY_UP) { recall(+1); return; }
    if (c == kbd::KEY_DOWN) { recall(-1); return; }
    if (c == '\n') {
        con::put_char('\n', FG, BG);
        if (s_len) {
            history_push(s_line);
            s_hist_pos = s_hist_count;
            exec(s_line);
            s_len = 0; s_line[0] = 0;
        }
        s_browsing = false;
        prompt();
        return;
    }
    if (c == '\b') {
        if (s_len) { s_len--; s_line[s_len] = 0; con::backspace(); }
        s_browsing = false;
        return;
    }
    if (c == 0x03) { con::print("^C\n"); s_len = 0; s_line[0] = 0; s_browsing = false; prompt(); return; } // Ctrl+C
    if (c == 0x0C) { c_clear(0, nullptr); prompt(); return; } // Ctrl+L
    if (c == 0x15) { erase_echo(); s_browsing = false; return; } // Ctrl+U
    if (c >= 0x20 && c < 0x7F) {
        if (s_len + 1 < LINE_MAX) {
            s_line[s_len++] = c;
            s_line[s_len] = 0;
            con::put_char(c, FG, BG);
        }
        s_browsing = false;
    }
    // other control codes: ignore
}

void status_bar() {
    con::status("up %llu s | A %llu | B %llu | sw %llu | type 'help'",
                pit::seconds(), s_ca.n, s_cb.n, sched::switches());
}

} // namespace

void start_workers() {
    if (!sched::spawn("counter-A", counter_main, &s_ca))
        con::println("[shell] spawn counter-A FAILED", ERR);
    if (!sched::spawn("counter-B", counter_main, &s_cb))
        con::println("[shell] spawn counter-B FAILED", ERR);
}

void run() {
    // Proofs: timer flowing and both workers scheduled at least once.
    uint64_t t0 = pit::ticks(), spin = 0;
    while (pit::ticks() == t0 && spin++ < 200000000ULL) { asm volatile("pause"); }
    if (pit::ticks() == t0) {
        con::println("[pit] FAIL: no ticks (IRQ0 missing). Check PIC/QEMU.", ERR);
        for (;;) { asm volatile("hlt"); }
    }
    uint64_t w0 = 0;
    while ((s_ca.n == 0 || s_cb.n == 0) && w0++ < 200000000ULL) { asm volatile("pause"); }
    if (s_ca.n == 0 || s_cb.n == 0) {
        con::println("[sched] FAIL: a counter never ran. Check timer preemption.", ERR);
        for (;;) { asm volatile("hlt"); }
    }
    con::println("[sched] preemptive multitasking live.", OK);

    con::println();
    con::println("Type 'help' for commands. Up/Down browse history.", ACC);
    prompt();

    uint64_t last_sec = pit::seconds();
    status_bar();
    for (;;) {
        net::poll();  // passo 1: answer ARP/ICMP even when idle
        char c;
        while (kbd::pop(c)) {
            // A focused user window owns the keyboard (display server).
            if (!gui::route_key(c)) handle_key(c);
            status_bar();
        }
        uint64_t s = pit::seconds();
        if (s != last_sec) { last_sec = s; status_bar(); }
        gui::tick(); // mouse gestures, 1 Hz clock, shortcut rescan, repaint
        asm volatile("hlt"); // sleep until the next IRQ
    }
}

} // namespace choco::shell

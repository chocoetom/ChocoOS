// Interactive shell: line editing, history, built-in commands.
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/gui.hpp>
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

struct Counter { const char *name; volatile uint64_t n = 0; };
Counter s_ca{"A"}, s_cb{"B"};

void counter_main(void *arg) {
    auto *c = (Counter *)arg;
    for (;;) {
        c->n = c->n + 1;
        if ((c->n & 0x3FFFFFULL) == 0) sched::yield(); // be nice
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
        "  ls                   list initrd files",
        "  cat <path>           print a file",
        "  run <path> [args]   run a user program in ring 3",
        "  gui                  toggle the system monitor window",
        "  mpos                 show mouse position and buttons",
        "  mclick <x> <y>       synthetic mouse click (UI testing)",
        "  wintest              synthetic drag test (moves terminal)",
        "  wallpaper [N]        list/select landscape wallpapers",
        "  ifconfig             show MAC/IP/gateway/net stats",
        "  ping <ip> [n]        ICMP echo to IPv4 (try 'ping 10.0.2.2')",
        "  arp                  show ARP table",
        "  dhcp [renew]         DHCP status (renews in background)",
        "  udpstat              UDP sockets and counters",
        "  dns <name>           resolve A records via UDP (try 'dns example.com')",
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

void c_gui(int, char **) {
    gui::toggle_sysmon();
    con::kprintf(FG, BG, "sysmon window %s (right-click also toggles)\n",
                 gui::sysmon_visible() ? "shown" : "hidden");
}

// Deterministic gesture test: synthesizes pointer motion through
// mouse::inject and checks the terminal window actually moved.
void c_wintest(int, char **) {
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

    // Resize: grab the right edge and drag it 60px outward.
    int w0 = 0, h0 = 0;
    gui::term_size(w0, h0);
    const int ex = x1 + w0 - 1, ey = y1 + h0 / 2;
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
    int w1 = 0, h1 = 0;
    gui::term_size(w1, h1);
    const bool rok = (w1 == w0 + 60) && (h1 == h0);
    con::kprintf(rok ? OK : ERR, BG, "[wintest] resize %dx%d -> %dx%d: %s\n", w0, h0, w1, h1,
                 rok ? "PASS (edge drag)" : "FAIL");

    // Maximize: double-click the caption.
    const int cx = x1 + 100, cy = y1 + 10;
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
    const bool rok2 = (wr == w1) && (hr == h1) && (xr == x1) && (yr == y1);
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

void c_ls(int, char **) {
    if (!fs::count()) { con::println("(initrd empty: no boot module found)", ERR); return; }
    for (size_t i = 0; i < fs::count(); i++) {
        const char *n = nullptr;
        size_t sz = 0;
        if (fs::stat(i, &n, &sz))
            con::kprintf(FG, BG, "  %s (%u B)\n", n, (unsigned)sz);
    }
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

void c_run(int argc, char **argv) {
    if (argc < 2) { con::println("usage: run <path> [args...]  (try 'run bin/hello foo')", ERR); return; }
    long code = proc::run_args(argv[1], argc - 2, &argv[2]);
    (void)code; // run_args already reported the outcome
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
    {"run", "run a user program", c_run},
    {"gui", "toggle sysmon window", c_gui},
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
    {"pci", "list PCI devices", c_pci},
};

// Try to run a program from initrd: handles "./x" and bare "x" (tries bin/x).
bool try_run_program(int argc, char **argv) {
    const char *name = argv[0];
    char path[LINE_MAX];
    const void *data = nullptr;
    size_t size = 0;

    // Case 1: explicit path with "./" prefix -> strip "./"
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
        // Try as-is relative to initrd root (e.g. "./bin/hello" -> "bin/hello")
        if (fs::read(name, &data, &size)) {
            proc::run_args(name, argc - 1, argv + 1);
            return true;
        }
        // Try with bin/ prefix (e.g. "./micro" -> "bin/micro")
        size_t i = 0;
        const char *prefix = "bin/";
        while (*prefix && i < LINE_MAX - 1) path[i++] = *prefix++;
        const char *p = name;
        while (*p && i < LINE_MAX - 1) path[i++] = *p++;
        path[i] = 0;
        if (fs::read(path, &data, &size)) {
            proc::run_args(path, argc - 1, argv + 1);
            return true;
        }
        return false;
    }

    // Case 2: explicit path containing '/' (e.g. bin/hello) -> run as-is
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            if (fs::read(name, &data, &size)) {
                proc::run_args(name, argc - 1, argv + 1);
                return true;
            }
            return false;
        }
    }

    // Case 3: bare name (e.g. "micro") -> try bin/<name>
    size_t i = 0;
    const char *prefix = "bin/";
    while (*prefix && i < LINE_MAX - 1) path[i++] = *prefix++;
    const char *p = name;
    while (*p && i < LINE_MAX - 1) path[i++] = *p++;
    path[i] = 0;
    if (fs::read(path, &data, &size)) {
        proc::run_args(path, argc - 1, argv + 1);
        return true;
    }
    return false;
}

void exec(char *line) {
    char *argv[ARGS_MAX];
    size_t argc = 0;
    for (char *p = line; *p && argc < ARGS_MAX;) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!argc) return;
    for (auto &c : kCmds)
        if (!util::strcmp(argv[0], c.name)) { c.fn((int)argc, argv); return; }
    // Fallback: try to run as user program (bin/<name>, ./name, or path)
    if (try_run_program((int)argc, argv)) return;
    con::kprintf(ERR, BG, "unknown command '%s' (try 'help' or 'ls')\n", argv[0]);
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
        gui::tick(); // mouse gestures, 1 Hz sysmon/taskbar, repaint
        asm volatile("hlt"); // sleep until the next IRQ
    }
}

} // namespace choco::shell

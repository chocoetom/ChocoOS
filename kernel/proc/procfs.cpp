// procfs: Linux-style virtual files, generated on demand.
//
// This is the data source for the user-space system monitor (user/sysmon.c) and
// for any ported Linux program: everything a monitor needs is a file, so ring 3
// never needs a privileged syscall. Content is integer-formatted only - the
// kernel is built with -mno-80387 -mno-sse, so there is no float here.
//
// Two rules this file has to keep:
//  * stat() reports size 0 and generates nothing. Virtual files have no stable
//    length, and the old implementation regenerated the buffer on every stat(),
//    which let a directory listing rewrite /proc content under a running reader.
//  * read() regenerates. A caller therefore sees a snapshot taken at open() and
//    must reopen for the next sample (that is also how Linux behaves for the
//    tools that poll).
//
// Nothing is invented: a metric with no kernel source is simply absent. That is
// why there is no loadavg (no run-queue depth), no temperature or battery (no
// ACPI), no GPU usage (no GPU driver) and no swap (no swap).
#include <choco/console.hpp>
#include <choco/boot.hpp>
#include <choco/cpu.hpp>
#include <choco/cpuinfo.hpp>
#include <choco/fat32.hpp>
#include <choco/mem.hpp>
#include <choco/net.hpp>
#include <choco/sched.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

namespace choco::procfs {
namespace {

// ---- bounded writer ------------------------------------------------------
struct W {
    char *b;
    size_t cap;
    size_t n = 0;

    void c(char ch) {
        if (n + 1 < cap) b[n++] = ch;
    }
    void s(const char *t) {
        for (; *t; t++) c(*t);
    }
    void u(uint64_t v) {
        char t[24];
        util::to_str(v, t);
        s(t);
    }
    // Fixed-width zero-padded hex, the way /proc prints addresses and states.
    void x(uint32_t v, int digits) {
        char t[9];
        for (int i = 0; i < 8; i++) {
            const uint32_t nib = (v >> ((7 - i) * 4)) & 0xF;
            t[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
        t[8] = 0;
        s(t + (8 - (digits > 8 ? 8 : digits)));
    }
    void left(const char *t, int width) {
        int used = 0;
        for (; *t && used < width; t++, used++) c(*t);
        while (used++ < width) c(' ');
    }
    void right(uint64_t v, int width) {
        char t[24];
        util::to_str(v, t);
        int len = 0;
        while (t[len]) len++;
        for (int i = len; i < width; i++) c(' ');
        s(t);
    }
    // value with two decimal digits, from an integer ratio (no float).
    void frac(uint64_t whole_num, uint64_t denom) {
        if (!denom) denom = 1;
        u(whole_num / denom);
        c('.');
        const uint64_t hundredths = ((whole_num % denom) * 100) / denom;
        if (hundredths < 10) c('0');
        u(hundredths);
    }
};

// ---- per-file buffers ----------------------------------------------------
char s_version[64]{}, s_meminfo[320]{}, s_uptime[64]{}, s_threads[1024]{};
char s_stat[256]{}, s_cpuinfo[512]{}, s_processes[1600]{}, s_diskstats[256]{};
char s_netdev[512]{}, s_netarp[640]{}, s_nettcp[640]{}, s_netudp[640]{};
char s_choco[640]{};

// CPU jiffies split the way /proc/stat wants them. The idle task is charged to
// "idle" rather than "system": without that, a busy-waiting hlt loop would make
// the machine read as 100% loaded all the time.
struct CpuJiffies {
    uint64_t user = 0, system = 0, idle = 0, total = 0;
};

CpuJiffies jiffies() {
    CpuJiffies j;
    for (size_t i = 0;; i++) {
        sched::ThreadInfo t;
        if (!sched::info(i, t)) break;
        if (t.idle) j.idle += t.cpu_ticks;
        else if (t.user) j.user += t.cpu_ticks;
        else j.system += t.cpu_ticks;
    }
    j.total = pit::ticks();
    return j;
}

size_t gen_version() {
    W w{s_version, sizeof(s_version)};
    w.s("ChocoOS 0.7 x86_64 (Limine)\n");
    return w.n;
}

size_t gen_meminfo() {
    W w{s_meminfo, sizeof(s_meminfo)};
    const uint64_t total = pmm::total_bytes() / 1024, freeb = pmm::free_bytes() / 1024;
    w.s("MemTotal: "); w.right(total, 8); w.s(" kB\n");
    w.s("MemFree: "); w.right(freeb, 8); w.s(" kB\n");
    // No page cache and no swap, so available really is free.
    w.s("MemAvailable: "); w.right(freeb, 8); w.s(" kB\n");
    w.s("KernelStack: "); w.right(heap::mapped_bytes() / 1024, 8); w.s(" kB\n");
    w.s("KernelHeapUsed: "); w.right(heap::used_bytes(), 8); w.s(" B\n");
    w.s("PageTables: "); w.right(pmm::used_bytes() / 1024, 8); w.s(" kB\n");
    return w.n;
}

size_t gen_uptime() {
    W w{s_uptime, sizeof(s_uptime)};
    const CpuJiffies j = jiffies();
    w.frac(j.total, pit::HZ);
    w.c(' ');
    w.frac(j.idle, pit::HZ);
    w.c('\n');
    return w.n;
}

size_t gen_threads() {
    W w{s_threads, sizeof(s_threads)};
    for (size_t i = 0;; i++) {
        const char *name = nullptr;
        uint64_t runs = 0;
        bool cur = false;
        if (!sched::stat(i, &name, &runs, &cur)) break;
        w.s(name ? name : "?");
        w.s(" runs=");
        w.u(runs);
        if (cur) w.s(" <-");
        w.c('\n');
    }
    return w.n;
}

size_t gen_stat() {
    W w{s_stat, sizeof(s_stat)};
    const CpuJiffies j = jiffies();
    // cpu user nice system idle iowait irq softirq steal guest guest_nice
    w.s("cpu  ");
    w.u(j.user); w.c(' '); w.u(0); w.c(' '); w.u(j.system); w.c(' '); w.u(j.idle);
    w.s(" 0 0 0 0 0 0\n");
    w.s("cpu0 ");
    w.u(j.user); w.c(' '); w.u(0); w.c(' '); w.u(j.system); w.c(' '); w.u(j.idle);
    w.s(" 0 0 0 0 0 0\n");
    w.s("ctxt "); w.u(sched::switches()); w.c('\n');
    return w.n;
}

size_t gen_cpuinfo() {
    W w{s_cpuinfo, sizeof(s_cpuinfo)};
    const cpuinfo::Info &c = cpuinfo::get();
    w.s("processor\t: 0\n");
    if (!c.ok) {
        w.s("vendor_id\t: unknown (CPUID not supported)\n");
        w.s("clock\t\t: "); w.u(pit::HZ); w.s(" Hz PIT, no TSC\n");
        return w.n;
    }
    w.s("vendor_id\t: "); w.s(c.vendor[0] ? c.vendor : "unknown"); w.c('\n');
    if (c.brand[0]) { w.s("model name\t: "); w.s(c.brand); w.c('\n'); }
    w.s("cpu family\t: "); w.u(c.family); w.c('\n');
    w.s("model\t\t: "); w.u(c.model); w.c('\n');
    w.s("stepping\t: "); w.u(c.stepping); w.c('\n');
    if (c.tsc_measured) {
        w.s("cpu MHz\t\t: ");
        w.u(c.tsc_khz / 1000); w.c('.');
        const uint32_t frac = c.tsc_khz % 1000;
        if (frac < 100) w.c('0');
        if (frac < 10) w.c('0');
        w.u(frac);
        w.c('\n');
    }
    w.s("cache size\t: 0 KB (no cache descriptor read)\n");
    w.s("features\t:");
    if (c.longmode) w.s(" lm");
    if (c.nx) w.s(" nx");
    if (c.sse2) w.s(" sse2");
    if (c.rdtsc) w.s(" tsc");
    w.c('\n');
    w.s("tick rate\t: "); w.u(pit::HZ); w.s(" Hz\n");
    w.s("cores\t\t: 1 (no SMP: the APIC/IOAPIC work is still open)\n");
    return w.n;
}

size_t gen_processes() {
    W w{s_processes, sizeof(s_processes)};
    w.s("  PID STATE CPUJIFFY   RUNS      MEM TYPE COMMAND\n");
    const uint32_t fg_pid = proc::foreground_pid();
    const uint64_t fg_data = proc::foreground_data_bytes();
    for (size_t i = 0;; i++) {
        sched::ThreadInfo t;
        if (!sched::info(i, t)) break;
        w.right(t.pid, 5);
        w.c(' ');
        w.c(t.state);
        w.c(' ');
        w.right(t.cpu_ticks, 8);
        w.right(t.runs, 8);
        // Only the foreground task has a heap worth reporting; kernel threads
        // show the stack the scheduler gave them.
        uint64_t mem = t.stack_bytes;
        if (t.user && t.pid == fg_pid) mem += fg_data;
        w.right(mem, 10);
        w.s(t.user ? " user " : " kern ");
        // The scheduler names user threads "user"/"clone"; the loader knows the
        // program, so prefer the real name for the running process.
        const char *nm = t.name;
        if (t.user && t.pid == fg_pid && proc::foreground_comm()[0]) nm = proc::foreground_comm();
        w.s(nm);
        w.c('\n');
    }
    return w.n;
}

size_t gen_diskstats() {
    W w{s_diskstats, sizeof(s_diskstats)};
    const fs::fat32::IoStats d = fs::fat32::io_stats();
    if (!d.disk) {
        w.s("# no ATA device answered IDENTIFY\n");
        return w.n;
    }
    // major minor name reads reads_merged sectors ms_read writes
    // writes_merged sectors_written ms_write in_flight ms_io weighted_ms
    w.s("  1   0 sda ");
    w.u(d.reads); w.c(' '); w.u(0); w.c(' '); w.u(d.read_sectors); w.c(' '); w.u(0);
    w.c(' '); w.u(d.writes); w.c(' '); w.u(0); w.c(' '); w.u(d.write_sectors);
    w.c(' '); w.u(0); w.s(" 0 0 0");
    if (d.errors) { w.c(' '); w.u(d.errors); w.s(" (failed transfers)"); }
    w.c('\n');
    return w.n;
}

size_t gen_netdev() {
    W w{s_netdev, sizeof(s_netdev)};
    const net::Stats &s = net::stats();
    w.s("Inter-|   Receive                                                |  Transmit\n");
    w.s(" face |bytes    packets errs drop fifo frame compressed multicast|bytes    "
        "packets errs drop fifo colls carrier compressed\n");
    const char *name = net::if_name();
    if (!name[0]) {
        w.s("# no NIC claimed at boot\n");
        return w.n;
    }
    w.left(name, 6); w.c(':');
    w.right(s.rx_bytes, 9); w.right(s.rx, 9);
    w.right(0, 5); w.right(s.dropped, 5);
    w.s("    0    0          0          0");
    w.right(s.tx_bytes, 9); w.right(s.tx, 9);
    w.right(0, 5); w.right(0, 5); w.s("    0     0       0          0");
    w.c('\n');
    return w.n;
}

void ip_hex(W &w, uint32_t ip) { w.x(ip, 8); }

size_t gen_netarp() {
    W w{s_netarp, sizeof(s_netarp)};
    w.s("IP address       HW type     Flags       HW address            Mask     Device\n");
    const char *name = net::if_name();
    for (int i = 0;; i++) {
        uint32_t ip = 0;
        uint8_t mac[6]{};
        uint64_t last_ms = 0;
        if (!net::arp_at(i, &ip, mac, &last_ms)) break;
        char ib[16];
        net::fmt_ipv4(ip, ib);
        w.left(ib, 16);
        w.left("0x1", 12);
        w.left("0x2", 12);
        char mb[18];
        net::fmt_mac(mac, mb);
        w.left(mb, 18);
        w.left("*", 9);
        w.s(name[0] ? name : "-");
        w.c('\n');
    }
    return w.n;
}

// /proc/net/tcp prints the state as a hex code, like Linux. The names come from
// net::tcp::st_name, so this maps that exact set.
uint32_t tcp_state_code(const char *st) {
    if (st[0] == 'E') return 0x01;                  // ESTABLISHED
    if (st[0] == 'S') return 0x02;                  // SYN-SENT
    if (st[0] == 'F' && st[9] == '1') return 0x04;  // FIN-WAIT-1
    if (st[0] == 'F') return 0x05;                  // FIN-WAIT-2
    if (st[0] == 'C' && st[1] == 'L') return 0x08;  // CLOSE-WAIT
    if (st[0] == 'L') return 0x09;                  // LAST-ACK
    if (st[0] == 'C') return 0x07;                  // CLOSED
    return 0x0B;
}

size_t gen_nettcp() {
    W w{s_nettcp, sizeof(s_nettcp)};
    w.s("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
        "retrnsmt   uid  timeout inode\n");
    for (int i = 0; i < net::tcp::MAX_CONN; i++) {
        net::tcp::ConnInfo c;
        if (!net::tcp::conn_at(i, c)) break;
        w.right((uint64_t)i + 1, 4); w.s(": ");
        ip_hex(w, c.local_ip); w.c(':'); w.x(c.local_port, 4); w.c(' ');
        ip_hex(w, c.remote_ip); w.c(':'); w.x(c.remote_port, 4); w.c(' ');
        w.x(tcp_state_code(c.state), 2); w.c(' ');
        w.x(c.tx_pending, 8); w.c(':'); w.x(c.rx_pending, 8); w.c(' ');
        w.s("00:00000000 00000000     0        0 0\n");
    }
    return w.n;
}

size_t gen_netudp() {
    W w{s_netudp, sizeof(s_netudp)};
    w.s("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
        "retrnsmt   uid  timeout inode\n");
    for (int i = 0;; i++) {
        net::udp::SockInfo s;
        if (!net::udp::sock_at(i, s)) break;
        w.right((uint64_t)i + 1, 4); w.s(": ");
        ip_hex(w, net::ip()); w.c(':'); w.x(s.port, 4); w.c(' ');
        ip_hex(w, s.has_dgram ? s.from_ip : 0); w.c(':'); w.x(s.from_port, 4); w.c(' ');
        w.s("07 00000000:");
        w.x(s.has_dgram ? s.dgram_len : 0, 8); w.c(' ');
        w.s("00:00000000 00000000     0        0 0\n");
    }
    return w.n;
}

// One "key<TAB>value" per line, machine-first: the monitor parses it and `cat`
// still reads well. Free-form prose here would make the parser guess.
size_t gen_choco() {
    W w{s_choco, sizeof(s_choco)};
    const fs::Summary fs_s = fs::summary();
    const auto &bi = boot::info();
    w.s("kernel\t\tChocoOS 0.7\n");
    w.s("framebuffer\t");
    w.u(bi.fb_w); w.c('x'); w.u(bi.fb_h); w.c('\n');
    w.s("display\t\tframebuffer, no GPU driver\n");
    w.s("initrd_files\t"); w.u(fs_s.initrd); w.c('\n');
    w.s("writable_files\t"); w.u(fs_s.writable); w.c('\n');
    w.s("writable_bytes\t"); w.u(fs_s.writable_bytes); w.c('\n');
    w.s("fat32\t\t\t");
    if (!fs_s.fat_mounted) w.s("absent\n");
    else { w.s(fs_s.fat_rw ? "mounted_rw\n" : "mounted_ro\n"); }
    w.s("fat32_bytes\t"); w.u(fs_s.fat_bytes); w.c('\n');
    w.s("fat32_free\t"); w.u(fs_s.fat_free); w.c('\n');
    w.s("fat32_entries\t"); w.u(fs_s.fat); w.c('\n');
    w.s("net\t\t\t");
    if (!net::ready()) w.s("absent\n");
    else {
        w.s("up\n");
        w.s("net_driver\t"); w.s(net::if_driver()); w.c('\n');
        w.s("net_iface\t"); w.s(net::if_name()); w.c('\n');
        w.s("net_mtu\t\t"); w.u(net::if_mtu()); w.c('\n');
        char ib[16];
        net::fmt_ipv4(net::ip(), ib);
        w.s("net_address\t"); w.s(ib); w.c('\n');
        net::fmt_ipv4(net::gw(), ib);
        w.s("net_gateway\t"); w.s(ib); w.c('\n');
        net::fmt_ipv4(net::dns_server(), ib);
        w.s("net_dns\t\t"); w.s(ib); w.c('\n');
        w.s("net_source\t"); w.s(net::ip_source_str()); w.c('\n');
        char mb[18];
        net::fmt_mac(net::mac(), mb);
        w.s("net_hwaddr\t"); w.s(mb); w.c('\n');
    }
    // Explicitly absent, so the UI can say why instead of showing a blank card.
    w.s("sensors\t\tunavailable (no ACPI driver)\n");
    w.s("power\t\tunavailable (no battery device)\n");
    w.s("gpu\t\tunavailable (no GPU driver)\n");
    return w.n;
}

// ---- table ---------------------------------------------------------------
struct File {
    const char *name;
    char *buf;
    size_t cap;
    size_t (*gen)();
};

const File kFiles[] = {
    {"proc/version", s_version, sizeof(s_version), gen_version},
    {"proc/stat", s_stat, sizeof(s_stat), gen_stat},
    {"proc/meminfo", s_meminfo, sizeof(s_meminfo), gen_meminfo},
    {"proc/uptime", s_uptime, sizeof(s_uptime), gen_uptime},
    {"proc/cpuinfo", s_cpuinfo, sizeof(s_cpuinfo), gen_cpuinfo},
    {"proc/threads", s_threads, sizeof(s_threads), gen_threads},
    {"proc/processes", s_processes, sizeof(s_processes), gen_processes},
    {"proc/diskstats", s_diskstats, sizeof(s_diskstats), gen_diskstats},
    {"proc/net/dev", s_netdev, sizeof(s_netdev), gen_netdev},
    {"proc/net/arp", s_netarp, sizeof(s_netarp), gen_netarp},
    {"proc/net/tcp", s_nettcp, sizeof(s_nettcp), gen_nettcp},
    {"proc/net/udp", s_netudp, sizeof(s_netudp), gen_netudp},
    {"proc/choco", s_choco, sizeof(s_choco), gen_choco},
};
constexpr size_t NFILES = sizeof(kFiles) / sizeof(kFiles[0]);

const File *lookup(const char *path) {
    while (*path == '/') path++;
    for (size_t i = 0; i < NFILES; i++)
        if (!util::strcmp(path, kFiles[i].name)) return &kFiles[i];
    return nullptr;
}

} // namespace

size_t count() { return NFILES; }

bool stat(size_t i, const char **name_out, size_t *size_out) {
    if (i >= NFILES) return false;
    if (name_out) *name_out = kFiles[i].name;
    // Virtual files have no length until they are read; 0 is what Linux reports
    // and it keeps a listing from regenerating content as a side effect.
    if (size_out) *size_out = 0;
    return true;
}

bool read(const char *path, const void **data_out, size_t *size_out) {
    const File *f = lookup(path);
    if (!f) return false;
    const size_t n = f->gen();
    if (data_out) *data_out = f->buf;
    if (size_out) *size_out = n;
    return true;
}

} // namespace choco::procfs

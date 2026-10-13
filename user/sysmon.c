// /bin/sysmon - ChocoOS System Monitor (ring 3, display server client).
//
// Every number on screen is read from /proc (kernel/proc/procfs.cpp): no
// privileged syscall and no kernel-side painting, so this is an ordinary
// application - the same contract Linux monitors use.
//
// Look: Fluent/Win11 structure on the ChocoOS "Midnight Amethyst" palette -
// a navigation rail, cards with live charts, a status bar. Integer arithmetic
// only: ring 3 is built with -mno-80387 -mno-sse, so there is no float here.
//
// Keys (case-insensitive): 1..5 tabs, Tab / Left / Right cycle tabs, Space pause,
// C compact, L light/dark, R reset history, +/- sample interval, Q or Esc quit.
// Processes: Up/Down (or J/K) select, PgUp/PgDn page, Home/End jump, S cycles the
// sort, U sorts by CPU, M by memory, N by PID (again = reverse); the CPU / Mem / PID
// headers are clickable too. Network: the same keys scroll the connection list.
#include "lib.h"
#include "lib/gui.h"

#define RING_N 90            // 90 samples x 500 ms = 45 s of history
#define MAXPROC 64
#define MAXARP 8
#define MAXCONN 16
#define HDR_H 36
#define RAIL_FULL 168        // icons + labels
#define RAIL_MINI 56         // icons only
#define RAIL_COLLAPSE_W 560  // window narrower than this -> icon rail
// The display server allocates every window's shared buffer at this capacity
// (UW_MAX_W x UW_MAX_H in kernel/gui/wm.cpp, shared via lib.h) and refuses to
// grow past it, so drawing beyond it would write outside the mapped pages.
// W/H reflow to the size the WM hands us via UEV_RESIZE; CAP_W/CAP_H only cap
// that (and size the animation scratch buffer below).
#define CAP_W UW_MAX_W
#define CAP_H UW_MAX_H
#define STAT_H 26
#define PAD 12
#define CARD_R 8
#define ROW_H 22

static int W = 620;
static int H = 460;
static int RW = RAIL_FULL;     // current rail width (see apply_size)
static int full_w = 620, full_h = 460;   // size to return to when leaving compact

static struct uw_win *g_win;
static GuiSurface g_s;
static long g_addr = 0;
static int quit_now = 0;

// ---- theme ---------------------------------------------------------------
// Two full token sets: the app never hardcodes a colour, so light/dark is one
// pointer swap (T) and both keep the same structure.
// Theme "Midnight Amethyst" - roles, not just colours:
//   surfaces  bg_top/bg_bot (window), card, surface (controls), sel (selection)
//   text      text (values) > dim (labels, >=5:1) > faint (hints, >=4.5:1 on card)
//   accent    accent (scrollbar), accent_hi (card titles, selected tab bar)
//   series    colour = metric family, so it means the same thing on every tab:
//               cpu purple | mem cyan | disk read green, write lime (wr)
//               net down amber (rx), up pink (tx)
//   contrast  every text token >= 4.5:1 on card; every series >= 3.9:1 on the
//             chart background in both themes (WCAG: 4.5 text, 3.0 graphics).
typedef GuiTheme Theme;
static const Theme *T = &GUI_THEME_MIDNIGHT_DARK;

enum { TAB_OVER, TAB_PERF, TAB_PROC, TAB_DISK, TAB_NET, TAB_N };
static const char *kTabName[TAB_N] = {"Overview", "Performance", "Processes",
                                      "Storage", "Network"};
static const char *kTabIcon[TAB_N] = {"system-monitor", "code_icon", "memory_icon",
                                      "computer", "wifi_icon"};
static int tab = TAB_OVER;

// ---- sampled state -------------------------------------------------------
static unsigned h_cpu[RING_N], h_mem[RING_N], h_rx[RING_N], h_tx[RING_N];
static unsigned h_rd[RING_N], h_wr[RING_N];
static int ring_len = 0;      // valid samples (oldest first in the charts)

typedef struct { unsigned long user, system, idle, total, ctxt; } CpuRaw;
typedef struct { unsigned long total_kb, free_kb, heap_kb; } MemRaw;
typedef struct {
    unsigned long reads, writes, rsect, wsect;
    int present;
} DiskRaw;
typedef struct {
    unsigned long rx_b, tx_b, rx_p, tx_p, drops;
    int present;
    char name[8];
} NetRaw;

typedef struct {
    unsigned long pid, jiffy, runs, mem;
    char name[24];
    char state;
    int user;
    int cpu;                  // percent over the last interval
} Proc;

typedef struct { char ip[16], mac[20]; } Arp;

typedef struct {
    unsigned long lip, rip;
    unsigned long lport, rport;
    char state[14];
    int udp;
} Conn;

static CpuRaw g_cpu, p_cpu;
static MemRaw g_mem;
static DiskRaw g_disk, p_disk;
static NetRaw g_net, p_net;
static Proc g_procs[MAXPROC];
static int g_nproc = 0;
static int g_nproc_total = 0;  // every task /proc listed, even past MAXPROC
static int g_order[MAXPROC];   // display order: indices into g_procs
static unsigned long prev_jiff[MAXPROC];
static unsigned long prev_pid[MAXPROC];
static int prev_n = 0;
static Arp g_arp[MAXARP];
static int g_narp = 0;
static Conn g_conn[MAXCONN];
static int g_nconn = 0;

static char g_vendor[16] = "unknown", g_model[48] = "unknown";
static unsigned long g_mhz = 0, g_family = 0, g_modno = 0, g_step = 0;
static char g_feat[24] = "";
static unsigned long g_up_s = 0;
static unsigned g_fb_w = 0, g_fb_h = 0;
static unsigned long g_initrd = 0, g_wfiles = 0, g_wbytes = 0;
static unsigned long g_fat_total = 0, g_fat_free = 0, g_fat_entries = 0;
static char g_fat_state[24] = "not mounted";
static char g_driver[16] = "", g_iface[8] = "", g_mtu[8] = "";
static char g_ip[16] = "", g_gw[16] = "", g_dns[16] = "", g_hw[18] = "";
static int g_net_ready = 0;

static unsigned rx_rate, tx_rate, rd_rate, wr_rate, ctxt_rate;
static int have_prev = 0;
static int paused = 0;
static unsigned interval_ms = 500;
static int compact = 0;
static int scroll_proc = 0, scroll_net = 0, sel_proc = 0;
enum { SORT_PID, SORT_CPU, SORT_MEM };
static int sort_key = SORT_PID, sort_desc = 0;
static long g_sel_pid = -1;    // selection follows the process, not the row
static int g_prows = 8;        // rows the process table showed last frame
static unsigned long last_sample_ms = 0;
static int dirty = 1;          // redraw only when something changed
static unsigned long now_ms = 0;

// ---- text helpers --------------------------------------------------------
static int pre(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static unsigned long get_num(const char **p) {
    skip_ws(p);
    unsigned long v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (unsigned long)(**p - '0');
        (*p)++;
    }
    return v;
}

static unsigned long get_hex(const char **p) {
    skip_ws(p);
    unsigned long v = 0;
    for (;;) {
        const char c = **p;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        (*p)++;
    }
    return v;
}

static void get_word(const char **p, char *out, int cap) {
    skip_ws(p);
    int n = 0;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && n + 1 < cap) out[n++] = *(*p)++;
    out[n] = 0;
}

static void trim(char *s) {
    int n = 0;
    while (s[n]) n++;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) {
        int j = 0;
        for (; s[i]; i++, j++) s[j] = s[i];
        s[j] = 0;
    }
}

// value after "key:" or "key\t" on a line
static const char *after_colon(const char *line) {
    const char *p = line;
    while (*p && *p != ':' && *p != '\t') p++;   // end of the key
    // /proc aligns keys with tabs before the colon, and sometimes with two
    // ("cpu MHz\t\t: 3086.855"), so consume every separator before the value.
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    return p;
}

static void put_str(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int gfx_strlen_(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void num_str(char *out, unsigned long v) { gfx_itoa(out, v); }

// thousands-separated number, for the big figures on the cards
static void num_sep(char *out, int cap, unsigned long v) {
    char t[24];
    num_str(t, v);
    int n = 0;
    while (t[n]) n++;
    int p = 0, first = n % 3;
    if (!first) first = 3;
    for (int i = 0; i < n;) {
        for (int k = 0; k < first && i < n; k++, i++) {
            if (p + 1 < cap) out[p++] = t[i];
        }
        first = 3;
        if (i < n && p + 1 < cap) out[p++] = ',';
    }
    out[p] = 0;
}

static void fmt_rate(char *out, int cap, unsigned long bps) {
    char n[24];
    if (bps >= 1024UL * 1024UL) {
        num_str(n, bps / (1024UL * 1024UL));
        put_str(out, cap, n);
        int p = 0;
        while (out[p]) p++;
        if (p + 5 < cap) {
            out[p++] = '.';
            out[p++] = (char)('0' + (int)((bps / (102UL * 1024UL)) % 10));
            out[p++] = ' '; out[p++] = 'M'; out[p++] = 'B'; out[p++] = '/'; out[p++] = 's';
            out[p] = 0;
        }
    } else if (bps >= 1024UL) {
        num_str(n, bps / 1024UL);
        put_str(out, cap, n);
        int p = 0;
        while (out[p]) p++;
        if (p + 5 < cap) {
            out[p++] = '.';
            out[p++] = (char)('0' + (int)((bps / 102UL) % 10));
            out[p++] = ' '; out[p++] = 'K'; out[p++] = 'B'; out[p++] = '/'; out[p++] = 's';
            out[p] = 0;
        }
    } else {
        num_str(n, bps);
        put_str(out, cap, n);
        int p = 0;
        while (out[p]) p++;
        if (p + 4 < cap) {
            out[p++] = ' '; out[p++] = 'B'; out[p++] = '/'; out[p++] = 's';
            out[p] = 0;
        }
    }
}

static void fmt_secs(char *out, int cap, unsigned long s) {
    char b[24];
    int p = 0;
    if (s >= 3600) {
        num_str(b, s / 3600);
        put_str(out + p, cap - p, b);
        while (out[p]) p++;
        if (p + 2 < cap) { out[p++] = 'h'; out[p++] = ' '; }
        s %= 3600;
    }
    num_str(b, s / 60);
    put_str(out + p, cap - p, b);
    while (out[p]) p++;
    if (p + 2 < cap) { out[p++] = 'm'; out[p++] = ' '; }
    num_str(b, s % 60);
    put_str(out + p, cap - p, b);
    while (out[p]) p++;
    if (p + 2 < cap) { out[p++] = 's'; out[p] = 0; }
}

static void ip_str(unsigned long v, char *out) {
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        num_str(out + p, (v >> (i * 8)) & 0xFF);
        while (out[p]) p++;
        if (i && p + 1 < 16) out[p++] = '.';
    }
    out[p] = 0;
}

// ---- /proc readers -------------------------------------------------------
static char buf[8192];     // /proc/processes alone can pass 2 KiB

// Reopens every time: a virtual file is a snapshot taken at open(), so an
// already-open fd would keep serving the same bytes forever.
static int read_file(const char *path) {
    long fd = choco_open(path);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        const long room = (long)sizeof(buf) - 1 - total;
        if (room <= 0) break;
        const long n = choco_read((int)fd, buf + total, (unsigned long)room);
        if (n <= 0) break;
        total += n;
    }
    choco_close((int)fd);
    buf[total] = 0;
    return (int)total;
}

// Walks the buffer line by line, NUL-terminating each one.
static char *next_line(char **cur) {
    char *line = *cur;
    if (!line || !*line) return 0;
    char *p = line;
    while (*p && *p != '\n') p++;
    if (*p == '\n') {
        *p = 0;
        *cur = p + 1;
    } else {
        *cur = p;
    }
    return line;
}

static void parse_stat(void) {
    if (read_file("proc/stat") < 0) return;
    char *cur = buf;
    char *line;
    while ((line = next_line(&cur))) {
        const char *p = line;
        if (pre(line, "cpu ")) {
            p += 4;
            g_cpu.user = get_num(&p);
            get_num(&p);                       // nice (never used)
            g_cpu.system = get_num(&p);
            g_cpu.idle = get_num(&p);
            g_cpu.total = g_cpu.user + g_cpu.system + g_cpu.idle;
        } else if (pre(line, "ctxt ")) {
            p += 5;
            g_cpu.ctxt = get_num(&p);
        }
    }
}

static void parse_meminfo(void) {
    if (read_file("proc/meminfo") < 0) return;
    g_mem.total_kb = g_mem.free_kb = g_mem.heap_kb = 0;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        const char *v = after_colon(line);
        if (pre(line, "MemTotal")) g_mem.total_kb = get_num(&v);
        else if (pre(line, "MemFree")) g_mem.free_kb = get_num(&v);
        else if (pre(line, "KernelStack")) g_mem.heap_kb = get_num(&v);
    }
}

static void parse_uptime(void) {
    if (read_file("proc/uptime") < 0) return;
    const char *p = buf;
    g_up_s = get_num(&p);
}

static void parse_processes(void) {
    if (read_file("proc/processes") < 0) return;
    g_nproc = 0;
    g_nproc_total = 0;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        const char *p = line;
        skip_ws(&p);
        if (*p < '0' || *p > '9') continue;     // header line
        g_nproc_total++;                        // count past the table cap too
        if (g_nproc >= MAXPROC) continue;
        Proc *pr = &g_procs[g_nproc];
        pr->pid = get_num(&p);
        skip_ws(&p);
        pr->state = *p ? *p : 'R';
        p++;
        pr->jiffy = get_num(&p);
        pr->runs = get_num(&p);
        pr->mem = get_num(&p);
        char type[8];
        get_word(&p, type, sizeof(type));
        pr->user = type[0] == 'u';
        get_word(&p, pr->name, sizeof(pr->name));
        pr->cpu = 0;
        g_nproc++;
    }
}

static void parse_cpuinfo(void) {
    if (read_file("proc/cpuinfo") < 0) return;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        const char *v = after_colon(line);
        if (pre(line, "vendor_id")) put_str(g_vendor, sizeof(g_vendor), v);
        else if (pre(line, "model name")) put_str(g_model, sizeof(g_model), v);
        else if (pre(line, "cpu MHz")) g_mhz = get_num(&v);
        else if (pre(line, "cpu family")) g_family = get_num(&v);
        else if (pre(line, "model\t") || pre(line, "model  ")) g_modno = get_num(&v);
        else if (pre(line, "stepping")) g_step = get_num(&v);
        else if (pre(line, "features")) put_str(g_feat, sizeof(g_feat), v);
    }
}

static void parse_diskstats(void) {
    g_disk.present = 0;
    if (read_file("proc/diskstats") < 0) return;
    const char *p = buf;
    skip_ws(&p);
    if (*p == '#') return;                       // no device: comment only
    get_num(&p);                                 // major
    get_num(&p);                                 // minor
    char name[16];
    get_word(&p, name, sizeof(name));
    g_disk.reads = get_num(&p);
    get_num(&p);                                 // merged
    g_disk.rsect = get_num(&p);
    get_num(&p);                                 // ms reading
    g_disk.writes = get_num(&p);
    get_num(&p);                                 // merged
    g_disk.wsect = get_num(&p);
    g_disk.present = 1;
}

static void parse_netdev(void) {
    g_net.present = 0;
    if (read_file("proc/net/dev") < 0) return;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        char *colon = line;
        while (*colon && *colon != ':') colon++;
        if (!*colon || colon == line) continue;  // header or no data row
        *colon = 0;
        trim(line);
        put_str(g_net.name, sizeof(g_net.name), line);
        const char *p = colon + 1;
        g_net.rx_b = get_num(&p);
        g_net.rx_p = get_num(&p);
        get_num(&p);                             // rx errs
        g_net.drops = get_num(&p);
        get_num(&p); get_num(&p); get_num(&p); get_num(&p);
        g_net.tx_b = get_num(&p);
        g_net.tx_p = get_num(&p);
        g_net.present = 1;
        break;
    }
}

static void parse_arp(void) {
    g_narp = 0;
    if (read_file("proc/net/arp") < 0) return;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) && g_narp < MAXARP) {
        const char *p = line;
        if (*p < '0' || *p > '9') continue;
        Arp *a = &g_arp[g_narp];
        get_word(&p, a->ip, sizeof(a->ip));
        char tmp[12];
        get_word(&p, tmp, sizeof(tmp));          // hw type
        get_word(&p, tmp, sizeof(tmp));          // flags
        get_word(&p, a->mac, sizeof(a->mac));
        g_narp++;
    }
}

static void parse_conns(const char *path, int udp) {
    if (read_file(path) < 0) return;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        const char *p = line;
        skip_ws(&p);
        if (*p < '0' || *p > '9') continue;      // header
        if (g_nconn >= MAXCONN) break;
        Conn *c = &g_conn[g_nconn];
        c->udp = udp;
        get_num(&p);                             // sl
        skip_ws(&p);
        c->lip = get_hex(&p);
        if (*p == ':') p++;
        c->lport = get_hex(&p);
        c->rip = get_hex(&p);
        if (*p == ':') p++;
        c->rport = get_hex(&p);
        const unsigned long st = get_hex(&p);
        const char *nm = udp ? "BOUND"
                       : st == 0x01 ? "ESTABLISHED"
                       : st == 0x02 ? "SYN-SENT"
                       : st == 0x04 ? "FIN-WAIT-1"
                       : st == 0x05 ? "FIN-WAIT-2"
                       : st == 0x08 ? "CLOSE-WAIT"
                       : st == 0x09 ? "LAST-ACK"
                       : "CLOSED";
        put_str(c->state, sizeof(c->state), nm);
        g_nconn++;
    }
}

// A number held in a string: get_num wants a cursor, not the buffer itself.
static unsigned long num_of(const char *text) {
    const char *p = text;
    return get_num(&p);
}

static void parse_choco(void) {
    if (read_file("proc/choco") < 0) return;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        char *tab = line;
        while (*tab && *tab != '	' && *tab != ' ') tab++;
        if (!*tab) continue;
        *tab = 0;
        const char *key = line;
        const char *v = tab + 1;
        while (*v == ' ' || *v == '	') v++;
        char val[40];
        get_word(&v, val, sizeof(val));
        if (pre(key, "framebuffer")) {
            const char *q = val;
            g_fb_w = get_num(&q);
            if (*q == 'x') q++;
            g_fb_h = get_num(&q);
        } else if (pre(key, "initrd_files")) g_initrd = num_of(val);
        else if (pre(key, "writable_files")) g_wfiles = num_of(val);
        else if (pre(key, "writable_bytes")) g_wbytes = num_of(val);
        else if (pre(key, "fat32")) put_str(g_fat_state, sizeof(g_fat_state), val);
        else if (pre(key, "fat32_bytes")) g_fat_total = num_of(val);
        else if (pre(key, "fat32_free")) g_fat_free = num_of(val);
        else if (pre(key, "fat32_entries")) g_fat_entries = num_of(val);
        else if (pre(key, "net")) g_net_ready = pre(val, "up");
        else if (pre(key, "net_driver")) put_str(g_driver, sizeof(g_driver), val);
        else if (pre(key, "net_iface")) put_str(g_iface, sizeof(g_iface), val);
        else if (pre(key, "net_mtu")) put_str(g_mtu, sizeof(g_mtu), val);
        else if (pre(key, "net_address")) put_str(g_ip, sizeof(g_ip), val);
        else if (pre(key, "net_gateway")) put_str(g_gw, sizeof(g_gw), val);
        else if (pre(key, "net_dns")) put_str(g_dns, sizeof(g_dns), val);
        else if (pre(key, "net_hwaddr")) put_str(g_hw, sizeof(g_hw), val);
    }
}

#define RATE_CAP 0x40000000u      // 1 GiB/s: keeps (int)vmax safe in the charts

// Append to a series: fills from the left, then scrolls once RING_N is reached.
// Callers bump ring_len themselves after pushing every series.
static void ring_push(unsigned *a, unsigned v) {
    if (ring_len < RING_N) {
        a[ring_len] = v;
    } else {
        for (int i = 0; i + 1 < RING_N; i++) a[i] = a[i + 1];
        a[RING_N - 1] = v;
    }
}

// ---- process ordering ----------------------------------------------------
// g_procs stays in /proc order (the per-pid CPU deltas are matched by pid, not
// by slot); the table reads it through g_order.
static int proc_after(int a, int b) {       // 1 when a belongs below b
    const Proc *pa = &g_procs[a], *pb = &g_procs[b];
    long d;
    if (sort_key == SORT_CPU) d = (long)pa->cpu - (long)pb->cpu;
    else if (sort_key == SORT_MEM) d = (pa->mem > pb->mem) - (pa->mem < pb->mem);
    else d = (pa->pid > pb->pid) - (pa->pid < pb->pid);
    if (sort_desc) d = -d;
    if (d == 0) d = (pa->pid > pb->pid) - (pa->pid < pb->pid);   // stable tie-break
    return d > 0;
}

static void resort(void) {
    for (int i = 0; i < g_nproc; i++) g_order[i] = i;
    for (int i = 1; i < g_nproc; i++) {     // insertion sort: n <= MAXPROC
        const int v = g_order[i];
        int j = i - 1;
        while (j >= 0 && proc_after(g_order[j], v)) { g_order[j + 1] = g_order[j]; j--; }
        g_order[j + 1] = v;
    }
    int found = -1;
    if (g_sel_pid >= 0) {
        for (int i = 0; i < g_nproc; i++)
            if ((long)g_procs[g_order[i]].pid == g_sel_pid) { found = i; break; }
    }
    if (found >= 0) {
        sel_proc = found;
    } else {                                // selected task exited: keep the row
        if (sel_proc >= g_nproc) sel_proc = g_nproc - 1;
        if (sel_proc < 0) sel_proc = 0;
        g_sel_pid = g_nproc ? (long)g_procs[g_order[sel_proc]].pid : -1;
    }
}

static void set_sort(int key) {
    if (key == sort_key) sort_desc = !sort_desc;
    else { sort_key = key; sort_desc = key != SORT_PID; }
    resort();
    dirty = 1;
}

static void sample(void) {
    parse_stat();
    parse_meminfo();
    parse_uptime();
    parse_processes();
    parse_cpuinfo();
    parse_choco();
    parse_diskstats();
    parse_netdev();
    g_nconn = 0;
    parse_conns("proc/net/tcp", 0);
    parse_conns("proc/net/udp", 1);
    parse_arp();

    // Rates use the real time since the previous sample. The loop sleeps in 40 ms
    // steps, the tick is 10 ms and a pause can stretch the gap arbitrarily, so
    // dividing by interval_ms would be wrong whenever any of those kick in.
    const unsigned long t_ms = (unsigned long)choco_ticks() * 10;
    unsigned long el = (last_sample_ms && t_ms > last_sample_ms) ? t_ms - last_sample_ms
                                                                  : interval_ms;
    if (el < 10) el = 10;
    last_sample_ms = t_ms;

    unsigned cpu_pct = 0, mem_pct = 0;
    unsigned long dt = 0;
    if (have_prev && g_cpu.total >= p_cpu.total) {
        dt = g_cpu.total - p_cpu.total;
        const unsigned long di = g_cpu.idle >= p_cpu.idle ? g_cpu.idle - p_cpu.idle : 0;
        const unsigned long busy = dt > di ? dt - di : 0;
        cpu_pct = dt ? (unsigned)(busy * 100 / dt) : 0;
        if (cpu_pct > 100) cpu_pct = 100;
        ctxt_rate = g_cpu.ctxt >= p_cpu.ctxt
                        ? (unsigned)((g_cpu.ctxt - p_cpu.ctxt) * 1000 / el)
                        : 0;
    }
    if (g_mem.total_kb && g_mem.total_kb >= g_mem.free_kb) {
        mem_pct = (unsigned)((g_mem.total_kb - g_mem.free_kb) * 100 / g_mem.total_kb);
        if (mem_pct > 100) mem_pct = 100;
    }
    if (have_prev && g_net.present && p_net.present) {
        rx_rate = (unsigned)((g_net.rx_b >= p_net.rx_b ? g_net.rx_b - p_net.rx_b : 0) * 1000 / el);
        tx_rate = (unsigned)((g_net.tx_b >= p_net.tx_b ? g_net.tx_b - p_net.tx_b : 0) * 1000 / el);
    }
    if (have_prev && g_disk.present && p_disk.present) {
        rd_rate = (unsigned)((g_disk.rsect >= p_disk.rsect ? g_disk.rsect - p_disk.rsect : 0) * 512 * 1000 / el);
        wr_rate = (unsigned)((g_disk.wsect >= p_disk.wsect ? g_disk.wsect - p_disk.wsect : 0) * 512 * 1000 / el);
    }
    // Per-process share of the interval, from jiffy deltas matched by pid.
    for (int i = 0; i < g_nproc; i++) {
        Proc *pr = &g_procs[i];
        pr->cpu = 0;
        if (!have_prev || !dt) continue;
        for (int k = 0; k < prev_n; k++) {
            if (prev_pid[k] != pr->pid) continue;
            if (pr->jiffy >= prev_jiff[k]) {
                pr->cpu = (int)((pr->jiffy - prev_jiff[k]) * 100 / dt);
                if (pr->cpu > 100) pr->cpu = 100;
            }
            break;
        }
    }
    for (int i = 0; i < g_nproc; i++) {
        prev_pid[i] = g_procs[i].pid;
        prev_jiff[i] = g_procs[i].jiffy;
    }
    prev_n = g_nproc;
    p_cpu = g_cpu;
    p_disk = g_disk;
    p_net = g_net;
    have_prev = 1;

    // Ring: newest last, oldest first. Rates are stored raw: the charts
    // autoscale (rate_scale), so there is no fixed ceiling to clamp to.
    ring_push(h_cpu, cpu_pct);
    ring_push(h_mem, mem_pct);
    ring_push(h_rx, rx_rate > RATE_CAP ? RATE_CAP : rx_rate);
    ring_push(h_tx, tx_rate > RATE_CAP ? RATE_CAP : tx_rate);
    ring_push(h_rd, rd_rate > RATE_CAP ? RATE_CAP : rd_rate);
    ring_push(h_wr, wr_rate > RATE_CAP ? RATE_CAP : wr_rate);
    if (ring_len < RING_N) ring_len++;
    resort();
    now_ms = t_ms;
}

static void reset_history(void) {
    ring_len = 0;
    have_prev = 0;
    last_sample_ms = 0;
    rx_rate = tx_rate = rd_rate = wr_rate = ctxt_rate = 0;
}

// ---- drawing -------------------------------------------------------------
static unsigned scratch[CAP_W + 60];

static void bg(void) {
    gfx_vgrad(&g_s, 0, 0, W, H, T->bg_top, T->bg_bot);
    for (int x = 0; x < W; x += 80) {
        for (int y = 0; y < H; y += 80) {
            if (((x / 80 + y / 80) & 1) == 0) {
                gfx_blend_alpha(&g_s, x, y, 80, 80, T->surface, 3);
            }
        }
    }
}

static void card(int x, int y, int w, int h) {
    gfx_elevate(&g_s, x, y, w, h, CARD_R, 28);
    gfx_glass_panel(&g_s, x, y, w, h, CARD_R, T->card, T->border, 240);
}

static void card_title(int x, int y, const char *t, unsigned color) {
    gfx_text(&g_s, x, y, t, color);
}

// A label/value pair row inside a card.
static void kv(int x, int y_right, int y, const char *k, const char *v) {
    gfx_text(&g_s, x, y, k, T->dim);
    gfx_text_right(&g_s, y_right, y, v, T->text);
}

static void chart(int x, int y, int w, int h, const unsigned *v, int n, unsigned vmax,
                  unsigned stroke, unsigned fill) {
    gfx_vgrad(&g_s, x, y, w, h, T->surface, T->bg_bot);
    gfx_grid(&g_s, x, y, w, h, 6, 4, T->grid);
    if (n <= 0 || vmax == 0) {
        gfx_text(&g_s, x + 6, y + h / 2 - 9, "⌛ collecting...", T->faint);
        return;
    }
    gfx_area_chart(&g_s, x, y, w, h, v, n, (int)vmax, stroke, fill, 64, scratch,
                   (int)(sizeof(scratch) / sizeof(scratch[0])));
    gfx_hline(&g_s, x, y + h, w, T->rule);
    gfx_hline(&g_s, x, y, w, T->rule);
}

// Second series drawn over a chart() of the same rect and scale.
static void chart2(int x, int y, int w, int h, const unsigned *v, int n, unsigned vmax,
                   unsigned color) {
    if (n <= 0 || vmax == 0) return;
    gfx_area_chart(&g_s, x, y, w, h, v, n, (int)vmax, color, color, 64, scratch,
                   (int)(sizeof(scratch) / sizeof(scratch[0])));
}

// Autoscaled y range for a pair of rate series: the next power of two (bytes/s)
// above the largest sample, never under 1 KB/s. Round figures ("512.0 KB/s",
// "1.0 MB/s") keep the axis label honest and the grid lines stable.
static unsigned rate_scale(const unsigned *a, const unsigned *b, int n) {
    unsigned m = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] > m) m = a[i];
        if (b[i] > m) m = b[i];
    }
    unsigned sc = 1024;
    while (sc < m && sc < RATE_CAP) sc <<= 1;
    return sc;
}

// Small axis label on a translucent plate so the series never hides it.
static void scale_tag(int x, int y, const char *t) {
    gfx_blend_alpha(&g_s, x, y, gfx_text_w(t) + 8, 17, T->bg_bot, 190);
    gfx_text(&g_s, x + 4, y, t, T->faint);
}

// Two-series rate chart (rx+tx, read+write) on one shared autoscale.
static void chart_rate_ex(int x, int y, int w, int h, const unsigned *a, const unsigned *b,
                          unsigned ca, unsigned cb, int tag) {
    const unsigned vmax = rate_scale(a, b, ring_len);
    chart(x, y, w, h, a, ring_len, vmax, ca, ca);
    chart2(x, y, w, h, b, ring_len, vmax, cb);
    if (ring_len <= 0 || !tag) return;
    char t[24];
    fmt_rate(t, sizeof(t), vmax);
    scale_tag(x + 2, y + 2, t);
    if (h >= 56) {
        fmt_rate(t, sizeof(t), vmax / 2);
        scale_tag(x + 2, y + h / 2 - 8, t);
    }
}

static void chart_rate(int x, int y, int w, int h, const unsigned *a, const unsigned *b,
                       unsigned ca, unsigned cb) {
    chart_rate_ex(x, y, w, h, a, b, ca, cb, 1);
}

// Short charts keep the grid clean: the axis range goes in the card's title row.
static void rate_note(int x_right, int y, const unsigned *a, const unsigned *b) {
    if (ring_len <= 0) return;
    char t[24], n[40];
    fmt_rate(t, sizeof(t), rate_scale(a, b, ring_len));
    put_str(n, sizeof(n), "0 - ");
    put_str(n + 4, (int)sizeof(n) - 4, t);
    gfx_text_right(&g_s, x_right, y, n, T->faint);
}

// "o name  o name" legend, right-aligned on x_right.
static void legend2(int x_right, int y, const char *na, unsigned ca, const char *nb,
                    unsigned cb) {
    const int xb = x_right - gfx_text_w(nb);
    gfx_text(&g_s, xb, y, nb, T->dim);
    gfx_dot(&g_s, xb - 9, y + 8, 3, cb);
    const int xa = xb - 9 - 12 - gfx_text_w(na);
    gfx_text(&g_s, xa, y, na, T->dim);
    gfx_dot(&g_s, xa - 9, y + 8, 3, ca);
}

static void scale_labels(int x, int y, int h, const char *top, const char *mid) {
    gfx_text(&g_s, x, y, top, T->faint);
    gfx_text(&g_s, x, y + h / 2 - 9, mid, T->faint);
    gfx_text(&g_s, x, y + h - 18, "0", T->faint);
}

static void meter_row(int x, int y, int w, const char *label, const char *value,
                      int pct, unsigned color) {
    gfx_text(&g_s, x, y, label, T->dim);
    gfx_text_right(&g_s, x + w, y, value, T->text);
    gfx_round(&g_s, x, y + 18, w, 7, 3, T->hover, 200);
    int fw = (int)((long)w * pct / 100);
    if (fw < 3) fw = 3;
    if (fw > w) fw = w;
    gfx_round(&g_s, x, y + 18, fw, 7, 3, color, 240);
    if (fw > 2) gfx_fill(&g_s, x + fw - 2, y + 19, 1, 5, gfx_mix(color, 0xFFFFFFu, 80));
}

static int rail_mini(void) { return RW < RAIL_FULL; }

static void header(void) {
    gfx_blend_alpha(&g_s, 0, 0, W, HDR_H, T->surface, 240);
    gfx_fill(&g_s, 0, HDR_H - 1, W, 1, T->rule);
    gfx_text(&g_s, PAD, TXTY(0, HDR_H), kTabName[tab], T->text);
    int right = W - PAD;
    if (paused) {
        const int cw = gfx_text_w("PAUSED") + 16;
        gfx_round(&g_s, right - cw, (HDR_H - 22) / 2, cw, 22, 5, T->danger, 90);
        gfx_text(&g_s, right - cw + 8, TXTY(0, HDR_H), "PAUSED", T->text);
        right -= cw + 12;
    }
    char u[48], b[64];
    fmt_secs(u, sizeof(u), g_up_s);
    put_str(b, sizeof(b), "↑ ");
    put_str(b + 2, (int)sizeof(b) - 2, u);
    const int lx = PAD + gfx_text_w(kTabName[tab]) + 20;
    if (right - gfx_text_w(b) >= lx) gfx_text_right(&g_s, right, TXTY(0, HDR_H), b, T->faint);
}

static void rail(void) {
    const int mini = rail_mini();
    gfx_blend_alpha(&g_s, 0, HDR_H, RW, H - HDR_H - STAT_H, T->surface, 160);
    gfx_fill(&g_s, RW - 1, HDR_H, 1, H - HDR_H - STAT_H, T->rule);
    for (int i = 0; i < TAB_N; i++) {
        const int y = HDR_H + 10 + i * 40;
        const int on = i == tab;
        if (on) {
            gfx_round(&g_s, 6, y, RW - 14, 34, R_CTRL, T->sel, 240);
            gfx_round(&g_s, 7, y + 9, 2, 16, 1, T->accent_hi, 255);
        } else {
            gfx_round(&g_s, 6, y, RW - 14, 34, R_CTRL, T->hover, 80);
        }
        const int ix = mini ? (RW - 24) / 2 : 16;
        if (!gfx_icon_at(&g_s, kTabIcon[i], ix, y + 5)) {
            if (mini) {
                const char ini[2] = {kTabName[i][0], 0};
                gfx_text(&g_s, ix + 8, TXTY(y, 34), ini, on ? T->text : T->dim);
            } else {
                gfx_fill(&g_s, ix, y + 11, 12, 12, on ? T->accent_hi : T->faint);
            }
        }
        if (!mini) gfx_text(&g_s, 46, TXTY(y, 34), kTabName[i], on ? T->text : T->dim);
    }
    if (mini) return;
    static const char *const kKey[] = {"1-5", "Space", "C", "L", "S", "Arrows"};
    static const char *const kAct[] = {"Tabs", "Pause", "Compact", "Theme", "Sort", "Move"};
    const int nk = 6, step = 19;
    const int ny = H - STAT_H - 28 - nk * step;
    if (ny < HDR_H + 10 + TAB_N * 40 + 16) return;
    gfx_hline(&g_s, 12, ny - 10, RW - 24, T->rule);
    gfx_text(&g_s, 14, ny, "Keys", T->faint);
    for (int i = 0; i < nk; i++) {
        gfx_text(&g_s, 14, ny + 24 + i * step, kKey[i], T->text);
        gfx_text(&g_s, 78, ny + 24 + i * step, kAct[i], T->dim);
    }
}

// Append to a bounded string buffer; returns the new end so calls chain.
static char *cat(char *p, char *end, const char *t) {
    while (*t && p + 1 < end) *p++ = *t++;
    *p = 0;
    return p;
}

static char *cat_num(char *p, char *end, unsigned long v) {
    char n[24];
    num_str(n, v);
    return cat(p, end, n);
}

static void status(void) {
    const int y = H - STAT_H;
    gfx_blend_alpha(&g_s, 0, y, W, STAT_H, T->surface, 230);
    gfx_fill(&g_s, 0, y, W, 1, T->rule);
    char b[128];
    char *p = b, *end = b + sizeof(b);
    p = cat(p, end, "⏱ ");
    p = cat_num(p, end, interval_ms);
    p = cat(p, end, "ms  •  ");
    p = cat_num(p, end, (unsigned long)ring_len);
    p = cat(p, end, " samples  •  ");
    if (tab == TAB_PROC && g_nproc > 0) {
        const int last = scroll_proc + g_prows < g_nproc ? scroll_proc + g_prows : g_nproc;
        p = cat(p, end, "rows ");
        p = cat_num(p, end, (unsigned long)scroll_proc + 1);
        p = cat(p, end, "-");
        p = cat_num(p, end, (unsigned long)last);
        p = cat(p, end, " of ");
        p = cat_num(p, end, (unsigned long)g_nproc);
        if (g_nproc_total > g_nproc) {
            p = cat(p, end, " (");
            p = cat_num(p, end, (unsigned long)g_nproc_total);
            p = cat(p, end, " tasks)");
        }
    } else {
        if (g_nproc_total > g_nproc) {
            p = cat_num(p, end, (unsigned long)g_nproc);
            p = cat(p, end, " of ");
        }
        p = cat_num(p, end, (unsigned long)g_nproc_total);
        p = cat(p, end, " tasks");
    }
    p = cat(p, end, "  •  ");
    cat(p, end, paused ? "⏸ paused" : "● live");
    gfx_text(&g_s, PAD, TXTY(y, STAT_H), b, T->dim);
    char r[32];
    put_str(r, sizeof(r), g_net_ready ? g_iface : "no nic");
    if (gfx_text_w(b) + gfx_text_w(r) + 3 * PAD < W)
        gfx_text_right(&g_s, W - PAD, TXTY(y, STAT_H), r, T->faint);
}

// ---- tabs ----------------------------------------------------------------
static void tab_overview(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    const int colw = (cw - 12) / 2;
    const int x2 = x0 + colw + 12;
    const int bottom = H - STAT_H - PAD;
    const int h1 = 144, h2 = 116;
    int y = HDR_H + PAD;
    char b[32];

    card(x0, y, colw, h1);
    card_title(x0 + 12, y + 10, "◆ CPU", T->accent_hi);
    num_str(b, ring_len ? h_cpu[ring_len - 1] : 0);
    int n = 0;
    while (b[n]) n++;
    b[n++] = '%';
    b[n] = 0;
    gfx_text(&g_s, x0 + 12, y + 34, b, T->text);
    gfx_text(&g_s, x0 + 12 + gfx_text_w(b) + 8, y + 34, "utilization", T->faint);
    chart(x0 + 12, y + 64, colw - 24, h1 - 64 - 12, h_cpu, ring_len, 100, T->cpu, T->cpu);

    card(x2, y, colw, h1);
    card_title(x2 + 12, y + 10, "◆ Memory", T->accent_hi);
    const unsigned long used_kb =
        g_mem.total_kb >= g_mem.free_kb ? g_mem.total_kb - g_mem.free_kb : 0;
    char tot[24], used[24], pct[12];
    num_sep(tot, sizeof(tot), g_mem.total_kb / 1024);
    num_sep(used, sizeof(used), used_kb / 1024);
    num_str(pct, ring_len ? h_mem[ring_len - 1] : 0);
    int pn = 0;
    while (pct[pn]) pn++;
    pct[pn++] = '%';
    pct[pn] = 0;
    gfx_text(&g_s, x2 + 12, y + 34, used, T->text);
    gfx_text(&g_s, x2 + 12 + gfx_text_w(used) + 8, y + 34, "MiB used", T->faint);
    gfx_text_right(&g_s, x2 + colw - 12, y + 34, pct, T->text);
    char totm[40];
    put_str(totm, sizeof(totm), tot);
    put_str(totm + gfx_strlen_(tot), (int)sizeof(totm) - gfx_strlen_(tot), " MiB total");
    meter_row(x2 + 12, y + 62, colw - 24, "RAM", totm, (int)(ring_len ? h_mem[ring_len - 1] : 0), T->mem);
    chart(x2 + 12, y + 96, colw - 24, h1 - 96 - 12, h_mem, ring_len, 100, T->mem, T->mem);
    y += h1 + 12;

    card(x0, y, colw, h2);
    card_title(x0 + 12, y + 10, "◆ Storage", T->accent_hi);
    if (!g_disk.present) {
        gfx_text(&g_s, x0 + 12, y + 34, "no ATA device", T->faint);
        gfx_text(&g_s, x0 + 12, y + 54, "volume is the FAT32", T->faint);
        gfx_text(&g_s, x0 + 12, y + 74, "ramdisk image", T->faint);
    } else {
        char r[32], w[32];
        fmt_rate(r, sizeof(r), rd_rate);
        fmt_rate(w, sizeof(w), wr_rate);
        gfx_dot(&g_s, x0 + 18, y + 43, 3, T->disk);
        gfx_text(&g_s, x0 + 30, y + 34, "read", T->dim);
        gfx_text_right(&g_s, x0 + colw - 12, y + 34, r, T->text);
        gfx_dot(&g_s, x0 + 18, y + 63, 3, T->wr);
        gfx_text(&g_s, x0 + 30, y + 54, "write", T->dim);
        gfx_text_right(&g_s, x0 + colw - 12, y + 54, w, T->text);
        rate_note(x0 + colw - 12, y + 10, h_rd, h_wr);
        chart_rate_ex(x0 + 12, y + 80, colw - 24, h2 - 80 - 10, h_rd, h_wr, T->disk, T->wr, 0);
    }
    card(x2, y, colw, h2);
    card_title(x2 + 12, y + 10, "◆ Network", T->accent_hi);
    if (!g_net.present) {
        gfx_text(&g_s, x2 + 12, y + 34, "no NIC at boot", T->faint);
        gfx_text(&g_s, x2 + 12, y + 54, "run QEMU with -nic", T->faint);
    } else {
        char r[32], t[32];
        fmt_rate(r, sizeof(r), rx_rate);
        fmt_rate(t, sizeof(t), tx_rate);
        gfx_dot(&g_s, x2 + 18, y + 43, 3, T->rx);
        gfx_text(&g_s, x2 + 30, y + 34, "down", T->dim);
        gfx_text_right(&g_s, x2 + colw - 12, y + 34, r, T->text);
        gfx_dot(&g_s, x2 + 18, y + 63, 3, T->tx);
        gfx_text(&g_s, x2 + 30, y + 54, "up", T->dim);
        gfx_text_right(&g_s, x2 + colw - 12, y + 54, t, T->text);
        rate_note(x2 + colw - 12, y + 10, h_rx, h_tx);
        chart_rate_ex(x2 + 12, y + 80, colw - 24, h2 - 80 - 10, h_rx, h_tx, T->rx, T->tx, 0);
    }
    y += h2 + 12;

    const int hs = bottom - y;
    if (hs >= 60) {
        card(x0, y, cw, hs);
        int sy = y + 32;
        card_title(x0 + 12, y + 10, "◆ System", T->accent_hi);
        char line[112];
        char *p, *e = line + sizeof(line);
        p = line; *p = 0;
        p = cat(p, e, g_model);
        if (g_mhz) { p = cat(p, e, "  @  "); p = cat_num(p, e, g_mhz); p = cat(p, e, " MHz"); }
        gfx_text(&g_s, x0 + 12, sy, line, T->text);
        sy += 19;
        p = line; *p = 0;
        p = cat(p, e, "vendor ");
        p = cat(p, e, g_vendor);
        p = cat(p, e, "   family ");
        p = cat_num(p, e, g_family);
        p = cat(p, e, "   model ");
        p = cat_num(p, e, g_modno);
        p = cat(p, e, "   stepping ");
        p = cat_num(p, e, g_step);
        gfx_text(&g_s, x0 + 12, sy, line, T->dim);
        sy += 19;
        p = line; *p = 0;
        p = cat(p, e, "display ");
        p = cat_num(p, e, g_fb_w);
        p = cat(p, e, "x");
        p = cat_num(p, e, g_fb_h);
        p = cat(p, e, "   features ");
        p = cat(p, e, g_feat[0] ? g_feat : " none reported");
        gfx_text(&g_s, x0 + 12, sy, line, T->dim);
        // Right-aligned note only if it does not collide with the line.
        const char *note = "no thermal/battery/GPU driver";
        if (gfx_text_w(line) + gfx_text_w(note) + 36 < cw)
            gfx_text_right(&g_s, x0 + cw - 12, sy, note, T->faint);
    }
}

static void tab_perf(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    int y = HDR_H + PAD;
    card(x0, y, cw, 210);
    card_title(x0 + 12, y + 10, "◆ CPU History", T->accent_hi);
    char b[24];
    num_str(b, ring_len ? h_cpu[ring_len - 1] : 0);
    int n = 0;
    while (b[n]) n++;
    b[n++] = '%'; b[n] = 0;
    gfx_text_right(&g_s, x0 + cw - 12, y + 10, b, T->text);
    chart(x0 + 12, y + 36, cw - 60, 150, h_cpu, ring_len, 100, T->cpu, T->cpu);
    scale_labels(x0 + cw - 44, y + 36, 150, "100", "50");
    y += 222;

    card(x0, y, cw, H - STAT_H - y - PAD);
    const int c2 = (cw - 24 - 24) / 2;               // two wide columns: label + value never touch
    const int lx = x0 + 12, rx = lx + c2 + 24;
    char v[32];
    num_sep(v, sizeof(v), g_cpu.user);
    kv(lx, lx + c2, y + 12, "user jiffies", v);
    num_sep(v, sizeof(v), g_cpu.system);
    kv(rx, rx + c2, y + 12, "system jiffies", v);
    num_sep(v, sizeof(v), g_cpu.idle);
    kv(lx, lx + c2, y + 36, "idle jiffies", v);
    num_sep(v, sizeof(v), ctxt_rate);
    kv(rx, rx + c2, y + 36, "context switches/s", v);

    // Busy time per process over the whole life, as bars.
    int yy = y + 62;
    gfx_text(&g_s, x0 + 12, yy, "CPU time by process (lifetime jiffies)", T->dim);
    yy += 22;
    unsigned long maxj = 1;
    for (int i = 0; i < g_nproc; i++)
        if (g_procs[i].jiffy > maxj) maxj = g_procs[i].jiffy;
    for (int i = 0; i < g_nproc && yy + ROW_H < H - STAT_H - 8; i++) {
        gfx_text(&g_s, x0 + 12, yy, g_procs[i].name, T->text);
        const int barx = x0 + 130, barw = cw - 200;
        const int fill = (int)(g_procs[i].jiffy * (unsigned long)barw / maxj);
        gfx_fill(&g_s, barx, yy + 4, barw, 10, T->surface);
        gfx_fill(&g_s, barx, yy + 4, fill, 10, T->cpu);
        char nb[24];
        num_str(nb, g_procs[i].jiffy);
        gfx_text_right(&g_s, x0 + cw - 12, yy, nb, T->dim);
        yy += ROW_H;
    }
}

// Column geometry, shared by drawing and header hit-testing so they cannot drift.
// Numeric columns are right-aligned on *_r; text columns start at *_x.
typedef struct { int pid_r, name_x, name_end, state_x, cpu_r, jif_r, mem_r, show_jif, show_state; } ProcCols;
static ProcCols pc;

static void proc_cols(int x0, int cw) {
    pc.mem_r = x0 + cw - 14;
    pc.show_jif = cw >= 400;                     // narrow: drop Jiffies, then State
    pc.show_state = cw >= 330;
    pc.jif_r = pc.mem_r - gfx_text_w("99,999 KiB") - 14;
    const int left = pc.show_jif ? pc.jif_r - gfx_text_w("999,999")
                                 : pc.mem_r - gfx_text_w("99,999 KiB");
    pc.cpu_r = left - 14;
    const int cpu_l = pc.cpu_r - gfx_text_w("CPU %");
    pc.state_x = cpu_l - 12 - gfx_text_w("State");
    pc.name_end = (pc.show_state ? pc.state_x : cpu_l) - 10;
    pc.pid_r = x0 + 12 + gfx_text_w("9999");
    pc.name_x = pc.pid_r + 14;
}

// Which sortable header is under x? -1 when none.
static int proc_head_hit(int x) {
    if (x >= pc.mem_r - gfx_text_w("99,999 KiB") - 7) return SORT_MEM;
    if (x >= pc.cpu_r - gfx_text_w("CPU %") - 14 && x <= pc.cpu_r + 9) return SORT_CPU;
    if (x < pc.name_x - 4) return SORT_PID;
    return -1;
}

static void sort_arrow(int x, int y, int desc) {       // 7x4 triangle
    for (int r = 0; r < 4; r++) {
        const int w = desc ? 7 - 2 * r : 1 + 2 * r;
        gfx_hline(&g_s, x + (7 - w) / 2, y + r, w, T->accent_hi);
    }
}

static void proc_head(int y) {
    const unsigned cp = sort_key == SORT_PID ? T->accent_hi : T->dim;
    const unsigned cc = sort_key == SORT_CPU ? T->accent_hi : T->dim;
    const unsigned cm = sort_key == SORT_MEM ? T->accent_hi : T->dim;
    gfx_text_right(&g_s, pc.pid_r, y, "PID", cp);
    if (sort_key == SORT_PID) sort_arrow(pc.pid_r - gfx_text_w("PID") - 11, y + 7, sort_desc);
    gfx_text(&g_s, pc.name_x, y, "Process", T->dim);
    if (pc.show_state) gfx_text(&g_s, pc.state_x, y, "State", T->dim);
    gfx_text_right(&g_s, pc.cpu_r, y, "CPU %", cc);
    if (sort_key == SORT_CPU) sort_arrow(pc.cpu_r - gfx_text_w("CPU %") - 11, y + 7, sort_desc);
    if (pc.show_jif) gfx_text_right(&g_s, pc.jif_r, y, "Jiffies", T->dim);
    gfx_text_right(&g_s, pc.mem_r, y, "Memory", cm);
    if (sort_key == SORT_MEM) sort_arrow(pc.mem_r - gfx_text_w("Memory") - 11, y + 7, sort_desc);
}

static void tab_proc(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    proc_cols(x0, cw);
    card(x0, HDR_H + PAD, cw, H - HDR_H - STAT_H - 2 * PAD);
    int y = HDR_H + PAD + 10;
    proc_head(y);
    y += 20;
    gfx_hline(&g_s, x0 + 12, y, cw - 24, T->rule);
    y += 8;
    int rows = (H - STAT_H - PAD - y) / ROW_H;
    if (rows < 1) rows = 1;
    g_prows = rows;
    if (sel_proc >= g_nproc) sel_proc = g_nproc ? g_nproc - 1 : 0;
    if (sel_proc < scroll_proc) scroll_proc = sel_proc;
    if (sel_proc >= scroll_proc + rows) scroll_proc = sel_proc - rows + 1;
    if (scroll_proc > g_nproc - rows) scroll_proc = g_nproc - rows;   // window grew
    if (scroll_proc < 0) scroll_proc = 0;
    const int name_avail = pc.name_end - pc.name_x;
    for (int i = scroll_proc; i < g_nproc && i < scroll_proc + rows; i++) {
        const int ry = y + (i - scroll_proc) * ROW_H;
        const Proc *p = &g_procs[g_order[i]];
        if (i == sel_proc) gfx_round(&g_s, x0 + 8, ry - 3, cw - 16, ROW_H - 1, R_ROW, T->sel, 255);
        char b[32];
        num_str(b, p->pid);
        gfx_text_right(&g_s, pc.pid_r, ry, b, T->dim);
        if (name_avail >= 8) {
            char nm[24];
            put_str(nm, sizeof(nm), p->name[0] ? p->name : "-");
            int nl = 0;
            while (nm[nl]) nl++;
            if (gfx_text_w(nm) > name_avail) {                  // clip, mark with ".."
                while (nl > 3 && gfx_text_w(nm) > name_avail) nm[--nl] = 0;
                if (nl > 3) { nm[nl - 1] = '.'; nm[nl - 2] = '.'; }
            }
            gfx_text(&g_s, pc.name_x, ry, nm, T->text);
        }
        char st[2] = {p->state ? p->state : 'R', 0};
        if (pc.show_state) gfx_text(&g_s, pc.state_x, ry, st, p->state == 'Z' ? T->danger : T->dim);
        num_str(b, (unsigned long)p->cpu);
        int k = 0;
        while (b[k]) k++;
        b[k++] = '%';
        b[k] = 0;
        gfx_text_right(&g_s, pc.cpu_r, ry, b, p->cpu ? T->cpu : T->faint);
        if (pc.show_jif) {
            num_sep(b, sizeof(b), p->jiffy);
            gfx_text_right(&g_s, pc.jif_r, ry, b, T->dim);
        }
        char mb[32];
        num_sep(mb, sizeof(mb), p->mem / 1024);
        k = 0;
        while (mb[k]) k++;
        put_str(mb + k, (int)sizeof(mb) - k, " KiB");
        gfx_text_right(&g_s, pc.mem_r, ry, mb, T->dim);
    }
    if (!g_nproc) gfx_text(&g_s, x0 + 12, y + 6, "no processes", T->faint);
    gfx_scrollbar(&g_s, W - 8, HDR_H + PAD + 30, H - HDR_H - STAT_H - 2 * PAD - 30,
                  g_nproc, rows, scroll_proc, T->surface, T->accent);
}

static void tab_disk(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    int y = HDR_H + PAD;
    const int colw = (cw - 12) / 2;

    card(x0, y, colw, 150);
    card_title(x0 + 12, y + 10, "◆ FAT32 Volume", T->accent_hi);
    if (g_fat_total == 0) {
        gfx_text(&g_s, x0 + 12, y + 44, "not mounted", T->faint);
    } else {
        char used[24], tot[24];
        num_sep(used, sizeof(used), (g_fat_total - g_fat_free) / 1048576);
        num_sep(tot, sizeof(tot), g_fat_total / 1048576);
        const int pct = (int)((g_fat_total - g_fat_free) * 100 / g_fat_total);
        gfx_text(&g_s, x0 + 12, y + 34, used, T->text);
        gfx_text(&g_s, x0 + 12 + gfx_text_w(used) + 6, y + 38, "MiB used", T->faint);
        meter_row(x0 + 12, y + 66, colw - 24, "Capacity", tot, pct, T->disk);
        char e[24];
        num_str(e, g_fat_entries);
        kv(x0 + 12, x0 + colw - 12, y + 108, "entries", e);
        kv(x0 + 12, x0 + colw - 12, y + 128, "state", g_fat_state);
    }

    const int x2 = x0 + colw + 12;
    card(x2, y, colw, 150);
    card_title(x2 + 12, y + 10, "◆ ATA Device", T->accent_hi);
    if (g_disk.present) legend2(x2 + colw - 12, y + 10, "read", T->disk, "write", T->wr);
    if (!g_disk.present) {
        gfx_text(&g_s, x2 + 12, y + 34, "no device on the IDE channels", T->faint);
        gfx_text(&g_s, x2 + 12, y + 54, "counters stay empty", T->faint);
    } else {
        char b[32];
        num_str(b, g_disk.reads);
        kv(x2 + 12, x2 + colw - 12, y + 34, "read transfers", b);
        num_str(b, g_disk.rsect);
        kv(x2 + 12, x2 + colw - 12, y + 56, "sectors read", b);
        num_str(b, g_disk.writes);
        kv(x2 + 12, x2 + colw - 12, y + 78, "write transfers", b);
        num_str(b, g_disk.wsect);
        kv(x2 + 12, x2 + colw - 12, y + 100, "sectors written", b);
        chart_rate(x2 + 12, y + 116, colw - 24, 26, h_rd, h_wr, T->disk, T->wr);
    }
    y += 162;

    card(x0, y, cw, H - STAT_H - y - PAD);
    card_title(x0 + 12, y + 10, "◆ Throughput & RAM", T->accent_hi);
    char r[32], w[32];
    fmt_rate(r, sizeof(r), rd_rate);
    fmt_rate(w, sizeof(w), wr_rate);
    const int c2w = (cw - 24 - 12) / 2;           // two inner columns
    const int rcol = x0 + 12 + c2w + 12;
    gfx_dot(&g_s, x0 + 16, y + 44, 3, T->disk);
    kv(x0 + 26, x0 + 12 + c2w, y + 36, "read", r);
    gfx_dot(&g_s, rcol + 4, y + 44, 3, T->wr);
    kv(rcol + 14, rcol + c2w, y + 36, "write", w);
    char v[32];
    num_str(v, g_wfiles);
    kv(x0 + 12, x0 + cw - 12, y + 66, "writable files (RAM overlay)", v);
    char sz[32];
    gfx_fmt_size(sz, g_wbytes);
    kv(x0 + 12, x0 + cw - 12, y + 88, "writable bytes", sz);
    num_str(v, g_initrd);
    kv(x0 + 12, x0 + cw - 12, y + 110, "initrd files", v);
    const int chh = H - STAT_H - y - PAD - 146;
    if (chh >= 20) {
        chart_rate(x0 + 12, y + 134, cw - 24, chh, h_rd, h_wr, T->disk, T->wr);
    }
}

static void tab_net(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    int y = HDR_H + PAD;
    const int c2w = (cw - 24 - 12) / 2;           // two inner columns
    const int lcol = x0 + 12;
    const int rcol = lcol + c2w + 12;
    card(x0, y, cw, 100);
    card_title(x0 + 12, y + 10, "◆ Network Adapter", T->accent_hi);
    if (!g_net_ready) {
        gfx_text(&g_s, x0 + 12, y + 36, "No NIC was claimed at boot.", T->dim);
        gfx_text(&g_s, x0 + 12, y + 58, "Run QEMU with -nic user,model=virtio", T->faint);
        y += 112;
    } else {
        char im[24];
        char *ip_ = im, *ie = im + sizeof(im);
        ip_ = cat(ip_, ie, g_iface);
        ip_ = cat(ip_, ie, " / ");
        ip_ = cat(ip_, ie, g_mtu);
        kv(lcol, lcol + c2w, y + 32, "driver", g_driver);
        kv(rcol, rcol + c2w, y + 32, "iface / mtu", im);
        kv(lcol, lcol + c2w, y + 54, "address", g_ip);
        kv(rcol, rcol + c2w, y + 54, "gateway", g_gw);
        kv(lcol, lcol + c2w, y + 76, "hwaddr", g_hw);
        kv(rcol, rcol + c2w, y + 76, "dns", g_dns);
        y += 112;
    }

    card(x0, y, cw, 118);
    card_title(x0 + 12, y + 10, "◆ Traffic", T->accent_hi);
    char r[32], t[32], pk[32];
    fmt_rate(r, sizeof(r), rx_rate);
    fmt_rate(t, sizeof(t), tx_rate);
    gfx_dot(&g_s, lcol + 4, y + 40, 3, T->rx);
    kv(lcol + 14, lcol + c2w, y + 32, "down", r);
    gfx_dot(&g_s, rcol + 4, y + 40, 3, T->tx);
    kv(rcol + 14, rcol + c2w, y + 32, "up", t);
    num_str(pk, g_net.rx_p);
    kv(lcol, lcol + c2w, y + 52, "frames rx", pk);
    num_str(pk, g_net.tx_p);
    kv(rcol, rcol + c2w, y + 52, "frames tx", pk);
    rate_note(x0 + cw - 12, y + 10, h_rx, h_tx);
    chart_rate_ex(lcol, y + 74, cw - 24, 34, h_rx, h_tx, T->rx, T->tx, 0);
    y += 130;

    // Connections then ARP, filling the rest of the page.
    card(x0, y, cw, H - STAT_H - y - PAD);
    int yy = y + 10;
    card_title(x0 + 12, yy, "◆ Connections", T->accent_hi);
    yy += 22;
    if (!g_nconn) gfx_text(&g_s, x0 + 12, yy, "none", T->faint);
    for (int i = scroll_net; i < g_nconn && yy + ROW_H < H - STAT_H - 12; i++) {
        const Conn *c = &g_conn[i];
        char la[32], ra[32];
        ip_str(c->lip, la);
        int p = 0;
        while (la[p]) p++;
        la[p++] = ':';
        num_str(la + p, c->lport);
        ip_str(c->rip, ra);
        p = 0;
        while (ra[p]) p++;
        ra[p++] = ':';
        num_str(ra + p, c->rport);
        gfx_text(&g_s, x0 + 12, yy, c->udp ? "udp" : "tcp", T->faint);
        gfx_text(&g_s, x0 + 52, yy, la, T->text);
        gfx_text(&g_s, x0 + 190, yy, "->", T->faint);
        gfx_text(&g_s, x0 + 214, yy, ra, T->text);
        gfx_text_right(&g_s, x0 + cw - 12, yy, c->state, T->dim);
        yy += ROW_H;
    }
    if (g_narp) {
        yy += 6;
        gfx_hline(&g_s, x0 + 12, yy, cw - 24, T->rule);
        yy += 8;
        card_title(x0 + 12, yy, "◆ ARP Neighbours", T->accent_hi);
        yy += 22;
        for (int i = 0; i < g_narp && yy + ROW_H < H - STAT_H - 12; i++) {
            gfx_text(&g_s, x0 + 12, yy, g_arp[i].ip, T->text);
            gfx_text(&g_s, x0 + 150, yy, g_arp[i].mac, T->dim);
            yy += ROW_H;
        }
    }
    gfx_scrollbar(&g_s, W - 8, y + 40, H - STAT_H - y - PAD - 40, g_nconn + g_narp, 8,
                  scroll_net, T->surface, T->accent);
}

static void render(void) {
    bg();
    if (compact) {
        char b[24];
        num_str(b, ring_len ? h_cpu[ring_len - 1] : 0);
        int n = 0;
        while (b[n]) n++;
        b[n++] = '%'; b[n] = 0;
        gfx_text(&g_s, PAD, 8, "◆ CPU", T->dim);
        gfx_text(&g_s, PAD + 50, 6, b, T->text);
        gfx_round(&g_s, PAD, 26, W - 2 * PAD, 9, 4, T->hover, 180);
        int mw = (int)((long)(W - 2 * PAD) * (ring_len ? h_cpu[ring_len - 1] : 0) / 100);
        gfx_round(&g_s, PAD, 26, mw, 9, 4, T->cpu, 240);
        num_str(b, ring_len ? h_mem[ring_len - 1] : 0);
        n = 0;
        while (b[n]) n++;
        b[n++] = '%'; b[n] = 0;
        gfx_text(&g_s, PAD, 44, "◆ RAM", T->dim);
        gfx_text(&g_s, PAD + 50, 42, b, T->text);
        gfx_round(&g_s, PAD, 62, W - 2 * PAD, 9, 4, T->hover, 180);
        mw = (int)((long)(W - 2 * PAD) * (ring_len ? h_mem[ring_len - 1] : 0) / 100);
        gfx_round(&g_s, PAD, 62, mw, 9, 4, T->mem, 240);
        chart(PAD, 80, W - 2 * PAD, H - 80 - 28, h_cpu, ring_len, 100, T->cpu, T->cpu);
        gfx_text(&g_s, PAD, H - 22, "⌨ 1-5 tabs   C full mode", T->faint);
        return;
    }
    switch (tab) {
        case TAB_OVER: tab_overview(); break;
        case TAB_PERF: tab_perf(); break;
        case TAB_PROC: tab_proc(); break;
        case TAB_DISK: tab_disk(); break;
        default: tab_net(); break;
    }
    header();
    rail();
    status();
}

// ---- events --------------------------------------------------------------
static void apply_size(int w, int h) {
    if (w < 200) w = 200;
    if (h < 140) h = 140;
    if (w > CAP_W) w = CAP_W;
    if (h > CAP_H) h = CAP_H;
    W = w;
    H = h;
    RW = W < RAIL_COLLAPSE_W ? RAIL_MINI : RAIL_FULL;
    if (!compact) { full_w = W; full_h = H; }
    g_s.w = W;
    g_s.h = H;
    // The kernel may change the row pitch when it resizes the surface.
    g_s.pitch = (int)g_win->pitch;
    if (g_s.pitch < W) g_s.pitch = W;
    dirty = 1;
}

// Navigation keys, after the raw feed has been decoded.
enum { K_UP = 0x1001, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END };

static void sel_set(int i) {
    if (g_nproc <= 0) { sel_proc = 0; g_sel_pid = -1; return; }
    if (i < 0) i = 0;
    if (i > g_nproc - 1) i = g_nproc - 1;
    sel_proc = i;
    g_sel_pid = (long)g_procs[g_order[i]].pid;
}

static void nav(int k) {
    dirty = 1;
    if (k == K_LEFT) { tab = (tab + TAB_N - 1) % TAB_N; scroll_net = 0; return; }
    if (k == K_RIGHT) { tab = (tab + 1) % TAB_N; scroll_net = 0; return; }
    if (tab == TAB_PROC) {
        const int page = g_prows > 1 ? g_prows - 1 : 1;
        switch (k) {
            case K_UP: sel_set(sel_proc - 1); break;
            case K_DOWN: sel_set(sel_proc + 1); break;
            case K_PGUP: sel_set(sel_proc - page); break;
            case K_PGDN: sel_set(sel_proc + page); break;
            case K_HOME: sel_set(0); break;
            case K_END: sel_set(g_nproc - 1); break;
        }
    } else if (tab == TAB_NET) {
        int v = scroll_net;
        switch (k) {
            case K_UP: v--; break;
            case K_DOWN: v++; break;
            case K_PGUP: v -= 8; break;
            case K_PGDN: v += 8; break;
            case K_HOME: v = 0; break;
            case K_END: v = g_nconn; break;
        }
        if (v > g_nconn - 1) v = g_nconn - 1;
        if (v < 0) v = 0;
        scroll_net = v;
    }
}

static void handle_key(char c) {
    dirty = 1;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);      // Shift / Caps Lock
    if (c == 'q') quit_now = 1;
    else if (c >= '1' && c <= '5') { tab = c - '1'; scroll_net = 0; }
    else if (c == '\t') tab = (tab + 1) % TAB_N;
    else if (c == ' ') paused = !paused;
    else if (c == 'c') {
        compact = !compact;
        const int nw = compact ? 320 : full_w, nh = compact ? 170 : full_h;
        const long r = choco_win_resize(g_addr, nw, nh);
        if (r < 0) {
            choco_print("sysmon: resize refused\n");
            compact = !compact;                // keep the UI in sync with the window
        } else {
            apply_size(nw, nh);                // do not wait for the resize event
        }
    } else if (c == 'l') {
        T = gui_theme_toggle(T);
    } else if (c == 'r') {
        reset_history();
    } else if (c == '+' || c == '=') {
        if (interval_ms < 2000) interval_ms += 250;
    } else if (c == '-' && interval_ms > 250) {
        interval_ms -= 250;
    } else if (c == 'j' || c == 2 || c == 10) {   // j / Ctrl-B / Enter = down
        nav(K_DOWN);
    } else if (c == 'k' || c == 8) {              // k / Backspace = up
        nav(K_UP);
    } else if (tab == TAB_PROC && c == 's') {
        set_sort((sort_key + 1) % 3);
    } else if (tab == TAB_PROC && c == 'u') {
        set_sort(SORT_CPU);
    } else if (tab == TAB_PROC && c == 'm') {
        set_sort(SORT_MEM);
    } else if (tab == TAB_PROC && c == 'n') {
        set_sort(SORT_PID);
    }
}

// The key feed is a character stream, so arrows / PgUp / PgDn / Home / End
// arrive as ANSI sequences (ESC [ A, ESC [ 5 ~, ESC O H ...). A bare ESC still
// quits, but only after a short wait proves no sequence follows it.
static int esc_state = 0;                    // 0 idle, 1 after ESC, 2 inside CSI/SS3
static unsigned esc_param = 0;
static int esc_semi = 0;
static unsigned long esc_t = 0;

static void on_key(unsigned info) {
    const unsigned long t = (unsigned long)choco_ticks() * 10;
    if (esc_state == 1) {
        if (info == '[' || info == 'O') {
            esc_state = 2; esc_param = 0; esc_semi = 0; esc_t = t;
            return;
        }
        esc_state = 0;                       // ESC + other key: drop the ESC, keep the key
    } else if (esc_state == 2) {
        if (info >= '0' && info <= '9') {
            if (!esc_semi) esc_param = esc_param * 10 + (info - '0');   // first parameter only
            return;
        }
        if (info == ';') { esc_semi = 1; return; }
        int k = 0;
        switch (info) {
            case 'A': k = K_UP; break;
            case 'B': k = K_DOWN; break;
            case 'C': k = K_RIGHT; break;
            case 'D': k = K_LEFT; break;
            case 'H': k = K_HOME; break;
            case 'F': k = K_END; break;
            case '~':
                k = esc_param == 1 || esc_param == 7 ? K_HOME
                  : esc_param == 4 || esc_param == 8 ? K_END
                  : esc_param == 5 ? K_PGUP
                  : esc_param == 6 ? K_PGDN : 0;
                break;
        }
        esc_state = 0;
        if (k) nav(k);
        return;
    }
#ifdef KEY_UP    // a kernel/lib that already decodes them hands us whole codes
    if (info == KEY_UP) { nav(K_UP); return; }
    if (info == KEY_DOWN) { nav(K_DOWN); return; }
    if (info == KEY_LEFT) { nav(K_LEFT); return; }
    if (info == KEY_RIGHT) { nav(K_RIGHT); return; }
#endif
#if defined(KEY_PGUP) && defined(KEY_PGDN) && defined(KEY_HOME) && defined(KEY_END)
    if (info == KEY_PGUP) { nav(K_PGUP); return; }
    if (info == KEY_PGDN) { nav(K_PGDN); return; }
    if (info == KEY_HOME) { nav(K_HOME); return; }
    if (info == KEY_END) { nav(K_END); return; }
#endif
    if (info == 27) { esc_state = 1; esc_t = t; return; }
    handle_key((char)info);
}

static void handle_click(int x, int y) {
    dirty = 1;
    if (!compact && x < RW && y >= HDR_H + 10) {
        const int i = (y - HDR_H - 10) / 40;
        if (i >= 0 && i < TAB_N) { tab = i; scroll_net = 0; }
        return;
    }
    if (!compact && tab == TAB_PROC) {
        const int x0 = RW + PAD;
        proc_cols(x0, W - x0 - PAD);
        const int top = HDR_H + PAD;
        if (y >= top + 4 && y < top + 34) {            // column header: sort
            const int k = proc_head_hit(x);
            if (k >= 0) set_sort(k);
            return;
        }
        if (y >= top + 38) {
            const int row = (y - (top + 38)) / ROW_H + scroll_proc;
            if (row >= 0 && row < g_nproc) sel_set(row);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    long addr = choco_win_create(W, H, "System Monitor");
    if (addr <= 0) {
        choco_print("sysmon: win_create failed\n");
        return 1;
    }
    g_addr = addr;
    g_win = (struct uw_win *)addr;
    g_s.px = (unsigned *)(addr + UW_PIX_OFF);
    g_s.w = W;
    g_s.h = H;
    g_s.pitch = (int)g_win->pitch;
    if (g_s.pitch < W) g_s.pitch = W;
    RW = W < RAIL_COLLAPSE_W ? RAIL_MINI : RAIL_FULL;

    sample();
    unsigned long next = (unsigned long)choco_ticks() * 10 + interval_ms;
    unsigned long last_draw = 0;
    while (1) {
        while (g_win->tail != g_win->head) {
            struct uw_ev e = g_win->ev[g_win->tail % 32];
            g_win->tail++;
            if (e.type == UEV_KEY) on_key((unsigned)e.info);
            else if (e.type == UEV_MOUSE) handle_click((int)e.x, (int)e.y);
            else if (e.type == UEV_RESIZE) apply_size((int)e.x, (int)e.y);
        }
        const unsigned long t = (unsigned long)choco_ticks() * 10;
        if (esc_state == 1 && t - esc_t >= 60) quit_now = 1;      // lone ESC
        else if (esc_state == 2 && t - esc_t >= 500) esc_state = 0;   // truncated sequence
        if (quit_now) {
            choco_win_close(addr);
            return 0;
        }
        if (!paused && t >= next) {
            sample();
            next = t + interval_ms;
            dirty = 1;
        }
        // Draw only when something changed; the 1 s refresh is a safety net in
        // case the display server needs an occasional present.
        if (dirty || t - last_draw >= 1000) {
            render();
            choco_win_present(addr);
            dirty = 0;
            last_draw = t;
        }
        choco_sleep(40);
    }
}

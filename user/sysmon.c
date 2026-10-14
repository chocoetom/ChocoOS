#include "lib.h"
#include "lib/gui.h"

#define RING_N 90
#define MAXPROC 64
#define MAXARP 8
#define MAXCONN 16
#define HDR_H 46
#define RAIL_FULL 184
#define RAIL_MINI 56
#define RAIL_COLLAPSE_W 600
#define STAT_H 28
#define PAD 14
#define GAP 12
#define HEAD_H 34
#define ROW_H 24
#define CAP_W UW_MAX_W
#define CAP_H UW_MAX_H
#define RATE_CAP 0x40000000u
#define SCR_N (UW_MAX_W + 60)

typedef GuiTheme Theme;

static const unsigned kFaint = 0x8E86A3u;
static const unsigned kWrite = 0x4FC3F7u;

static struct uw_win *g_win;
static GuiSurface g_s;
static long g_addr = 0;
static int quit_now = 0;

static int W = 680;
static int H = 540;
static int RW = RAIL_FULL;
static int full_w = 680;
static int full_h = 540;
static int compact = 0;

static const Theme *T = &GUI_THEME_MIDNIGHT_DARK;

enum { TAB_OVER, TAB_PERF, TAB_PROC, TAB_DISK, TAB_NET, TAB_N };
static const char *kTabName[TAB_N] = {"Overview", "Performance", "Processes", "Storage", "Network"};
static const char *kTabIcon[TAB_N] = {"system-monitor", "code_icon", "memory_icon", "computer", "wifi_icon"};
static const char *kTabHint[TAB_N] = {
    "Live summary of the machine",
    "CPU history and load distribution",
    "Tasks, state and resource use",
    "Volumes and ATA throughput",
    "Adapter, traffic and connections"};
static int tab = TAB_OVER;

static unsigned h_cpu[RING_N], h_mem[RING_N], h_rx[RING_N], h_tx[RING_N];
static unsigned h_rd[RING_N], h_wr[RING_N];
static int ring_len = 0;

typedef struct { unsigned long user, system, idle, total, ctxt; } CpuRaw;
typedef struct { unsigned long total_kb, free_kb, heap_kb; } MemRaw;
typedef struct { unsigned long reads, writes, rsect, wsect; int present; } DiskRaw;
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
    int cpu;
} Proc;
typedef struct { char ip[16], mac[20]; } Arp;
typedef struct {
    unsigned long lip, rip, lport, rport;
    char state[14];
    int udp;
} Conn;

static CpuRaw g_cpu, p_cpu;
static MemRaw g_mem;
static DiskRaw g_disk, p_disk;
static NetRaw g_net, p_net;
static Proc g_procs[MAXPROC];
static int g_nproc = 0;
static int g_nproc_total = 0;
static int g_order[MAXPROC];
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
static int scroll_proc = 0, scroll_net = 0, sel_proc = 0;
enum { SORT_PID, SORT_CPU, SORT_MEM };
static int sort_key = SORT_PID, sort_desc = 0;
static long g_sel_pid = -1;
static int g_prows = 8;
static unsigned long last_sample_ms = 0;
static int dirty = 1;

static int pre(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static int key_eq(const char *s, const char *k) {
    while (*k) {
        if (*s++ != *k++) return 0;
    }
    return *s == 0;
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

static const char *after_colon(const char *line) {
    const char *p = line;
    while (*p && *p != ':' && *p != '\t') p++;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    return p;
}

static void put_str(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void num_str(char *out, unsigned long v) { gfx_itoa(out, v); }

static void num_sep(char *out, int cap, unsigned long v) {
    char t[24];
    num_str(t, v);
    const int n = str_len(t);
    int p = 0;
    int first = n % 3;
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

static void fmt_pct(char *out, unsigned long v) {
    num_str(out, v);
    const int n = str_len(out);
    out[n] = '%';
    out[n + 1] = 0;
}

static void fmt_rate(char *out, int cap, unsigned long bps) {
    char n[24];
    int p;
    if (bps >= 1024UL * 1024UL) {
        num_str(n, bps / (1024UL * 1024UL));
        put_str(out, cap, n);
        p = str_len(out);
        if (p + 6 < cap) {
            out[p++] = '.';
            out[p++] = (char)('0' + (int)((bps / (102UL * 1024UL)) % 10));
            out[p++] = ' '; out[p++] = 'M'; out[p++] = 'B'; out[p++] = '/'; out[p++] = 's';
            out[p] = 0;
        }
    } else if (bps >= 1024UL) {
        num_str(n, bps / 1024UL);
        put_str(out, cap, n);
        p = str_len(out);
        if (p + 6 < cap) {
            out[p++] = '.';
            out[p++] = (char)('0' + (int)((bps / 102UL) % 10));
            out[p++] = ' '; out[p++] = 'K'; out[p++] = 'B'; out[p++] = '/'; out[p++] = 's';
            out[p] = 0;
        }
    } else {
        num_str(n, bps);
        put_str(out, cap, n);
        p = str_len(out);
        if (p + 4 < cap) {
            out[p++] = ' '; out[p++] = 'B'; out[p++] = '/'; out[p++] = 's';
            out[p] = 0;
        }
    }
}

static void fmt_uptime(char *out, int cap, unsigned long s) {
    char b[24];
    int p = 0;
    if (s >= 86400) {
        num_str(b, s / 86400);
        put_str(out + p, cap - p, b);
        p = str_len(out);
        if (p + 2 < cap) { out[p++] = 'd'; out[p++] = ' '; out[p] = 0; }
        s %= 86400;
    }
    num_str(b, s / 3600);
    put_str(out + p, cap - p, b);
    p = str_len(out);
    if (p + 2 < cap) { out[p++] = 'h'; out[p++] = ' '; out[p] = 0; }
    if (s / 60 % 60 < 10) { out[p++] = '0'; out[p] = 0; }
    num_str(b, (s / 60) % 60);
    put_str(out + p, cap - p, b);
    p = str_len(out);
    if (p + 2 < cap) { out[p++] = 'm'; out[p] = 0; }
}

static void ip_str(unsigned long v, char *out) {
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        num_str(out + p, (v >> (i * 8)) & 0xFF);
        p = str_len(out);
        if (i && p + 1 < 16) out[p++] = '.';
    }
    out[p] = 0;
}

static char buf[8192];

static int read_file(const char *path) {
    const long fd = choco_open(path);
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
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        const char *p = line;
        if (pre(line, "cpu ")) {
            p += 4;
            g_cpu.user = get_num(&p);
            get_num(&p);
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
        if (*p < '0' || *p > '9') continue;
        g_nproc_total++;
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
    if (*p == '#') return;
    get_num(&p);
    get_num(&p);
    char name[16];
    get_word(&p, name, sizeof(name));
    g_disk.reads = get_num(&p);
    get_num(&p);
    g_disk.rsect = get_num(&p);
    get_num(&p);
    g_disk.writes = get_num(&p);
    get_num(&p);
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
        if (!*colon || colon == line) continue;
        *colon = 0;
        trim(line);
        put_str(g_net.name, sizeof(g_net.name), line);
        const char *p = colon + 1;
        g_net.rx_b = get_num(&p);
        g_net.rx_p = get_num(&p);
        get_num(&p);
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
        get_word(&p, tmp, sizeof(tmp));
        get_word(&p, tmp, sizeof(tmp));
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
        if (*p < '0' || *p > '9') continue;
        if (g_nconn >= MAXCONN) break;
        Conn *c = &g_conn[g_nconn];
        c->udp = udp;
        get_num(&p);
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

static unsigned long num_of(const char *text) {
    const char *p = text;
    return get_num(&p);
}

static void parse_choco(void) {
    if (read_file("proc/choco") < 0) return;
    char *cur = buf, *line;
    while ((line = next_line(&cur))) {
        char *tab_at = line;
        while (*tab_at && *tab_at != '\t' && *tab_at != ' ') tab_at++;
        if (!*tab_at) continue;
        *tab_at = 0;
        const char *key = line;
        const char *v = tab_at + 1;
        while (*v == ' ' || *v == '\t') v++;
        char val[40];
        get_word(&v, val, sizeof(val));
        if (key_eq(key, "framebuffer")) {
            const char *q = val;
            g_fb_w = get_num(&q);
            if (*q == 'x') q++;
            g_fb_h = get_num(&q);
        } else if (key_eq(key, "initrd_files")) g_initrd = num_of(val);
        else if (key_eq(key, "writable_files")) g_wfiles = num_of(val);
        else if (key_eq(key, "writable_bytes")) g_wbytes = num_of(val);
        else if (key_eq(key, "fat32")) put_str(g_fat_state, sizeof(g_fat_state), val);
        else if (key_eq(key, "fat32_bytes")) g_fat_total = num_of(val);
        else if (key_eq(key, "fat32_free")) g_fat_free = num_of(val);
        else if (key_eq(key, "fat32_entries")) g_fat_entries = num_of(val);
        else if (key_eq(key, "net")) g_net_ready = pre(val, "up");
        else if (key_eq(key, "net_driver")) put_str(g_driver, sizeof(g_driver), val);
        else if (key_eq(key, "net_iface")) put_str(g_iface, sizeof(g_iface), val);
        else if (key_eq(key, "net_mtu")) put_str(g_mtu, sizeof(g_mtu), val);
        else if (key_eq(key, "net_address")) put_str(g_ip, sizeof(g_ip), val);
        else if (key_eq(key, "net_gateway")) put_str(g_gw, sizeof(g_gw), val);
        else if (key_eq(key, "net_dns")) put_str(g_dns, sizeof(g_dns), val);
        else if (key_eq(key, "net_hwaddr")) put_str(g_hw, sizeof(g_hw), val);
    }
}

static void ring_push(unsigned *a, unsigned v) {
    if (ring_len < RING_N) {
        a[ring_len] = v;
    } else {
        for (int i = 0; i + 1 < RING_N; i++) a[i] = a[i + 1];
        a[RING_N - 1] = v;
    }
}

static int proc_after(int a, int b) {
    const Proc *pa = &g_procs[a], *pb = &g_procs[b];
    long d;
    if (sort_key == SORT_CPU) d = (long)pa->cpu - (long)pb->cpu;
    else if (sort_key == SORT_MEM) d = (pa->mem > pb->mem) - (pa->mem < pb->mem);
    else d = (pa->pid > pb->pid) - (pa->pid < pb->pid);
    if (sort_desc) d = -d;
    if (d == 0) d = (pa->pid > pb->pid) - (pa->pid < pb->pid);
    return d > 0;
}

static void resort(void) {
    for (int i = 0; i < g_nproc; i++) g_order[i] = i;
    for (int i = 1; i < g_nproc; i++) {
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
    } else {
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

    const unsigned long t_ms = (unsigned long)choco_ticks() * 10;
    unsigned long el = (last_sample_ms && t_ms > last_sample_ms) ? t_ms - last_sample_ms : interval_ms;
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
        ctxt_rate = g_cpu.ctxt >= p_cpu.ctxt ? (unsigned)((g_cpu.ctxt - p_cpu.ctxt) * 1000 / el) : 0;
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

    ring_push(h_cpu, cpu_pct);
    ring_push(h_mem, mem_pct);
    ring_push(h_rx, rx_rate > RATE_CAP ? RATE_CAP : rx_rate);
    ring_push(h_tx, tx_rate > RATE_CAP ? RATE_CAP : tx_rate);
    ring_push(h_rd, rd_rate > RATE_CAP ? RATE_CAP : rd_rate);
    ring_push(h_wr, wr_rate > RATE_CAP ? RATE_CAP : wr_rate);
    if (ring_len < RING_N) ring_len++;
    resort();
}

static void reset_history(void) {
    ring_len = 0;
    have_prev = 0;
    last_sample_ms = 0;
    rx_rate = tx_rate = rd_rate = wr_rate = ctxt_rate = 0;
}

static unsigned scratch[SCR_N];

static unsigned last_of(const unsigned *v) {
    return ring_len ? v[ring_len - 1] : 0;
}

static void bg(void) {
    gfx_vgrad(&g_s, 0, 0, W, H, T->bg_top, T->bg_bot);
    gfx_round(&g_s, W - 300, -160, 420, 420, 210, T->accent, 16);
    gfx_round(&g_s, -180, H - 300, 380, 380, 190, T->cpu, 10);
    gfx_round(&g_s, W / 3, H - 120, 300, 260, 130, T->mem, 6);
}

static void card(int x, int y, int w, int h) {
    gfx_card(&g_s, x, y, w, h, T, 1);
}

static void card_head(int x, int y, int w, const char *title, unsigned accent, const char *right) {
    gfx_dot(&g_s, x + 16, y + 17, 3, accent);
    gfx_text(&g_s, x + 26, TXTY(y + 8, 18), title, T->text);
    if (right) gfx_text_right(&g_s, x + w - 14, TXTY(y + 8, 18), right, kFaint);
    gfx_hline(&g_s, x + 14, y + HEAD_H - 1, w - 28, T->rule);
}

static void kv_stack(int x, int y, int w, const char *label, const char *value, unsigned color) {
    gfx_text(&g_s, x, y, label, kFaint);
    gfx_text_clip(&g_s, x, y + 17, x + w, value, color);
}

static void kv_row(int x, int y, int w, const char *label, const char *value) {
    gfx_text(&g_s, x, TXTY(y, 18), label, T->dim);
    gfx_text_right(&g_s, x + w, TXTY(y, 18), value, T->text);
}

static int pill(int right, int y, const char *t, unsigned fill, unsigned ink) {
    const int w = gfx_text_w(t) + 22;
    gfx_round(&g_s, right - w, y, w, 22, 11, fill, 255);
    gfx_text(&g_s, right - w + 11, TXTY(y, 22), t, ink);
    return right - w - 8;
}

static void meter(int x, int y, int w, int pct, unsigned color) {
    int p = pct;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    gfx_round(&g_s, x, y, w, 8, 4, T->hover, 255);
    int fw = (int)((long)w * p / 100);
    if (fw > 0 && fw < 8) fw = 8;
    if (fw > w) fw = w;
    if (fw <= 0) return;
    gfx_round(&g_s, x, y, fw, 8, 4, color, 255);
    gfx_hline(&g_s, x + 4, y + 1, fw > 8 ? fw - 8 : 1, gfx_mix(color, 0xFFFFFFu, 110));
}

static void meter_row(int x, int y, int w, const char *label, const char *value, int pct, unsigned color) {
    gfx_text(&g_s, x, TXTY(y, 18), label, T->dim);
    gfx_text_right(&g_s, x + w, TXTY(y, 18), value, T->text);
    meter(x, y + 22, w, pct, color);
}

static void chart_frame(int x, int y, int w, int h) {
    gfx_blend_alpha(&g_s, x, y, w, h, T->bg_bot, 110);
    for (int j = 1; j < 4; j++) {
        const int yy = y + h * j / 4;
        for (int i = x; i < x + w; i += 4) gfx_blend(&g_s, i, yy, T->grid, 230);
    }
    for (int i = 1; i < 6; i++) {
        const int xx = x + w * i / 6;
        for (int j = y; j < y + h; j += 4) gfx_blend(&g_s, xx, j, T->grid, 210);
    }
    gfx_hline(&g_s, x, y + h - 1, w, T->rule);
}

static void series_area(int x, int y, int w, int h, const unsigned *v, int n, unsigned vmax, unsigned color) {
    if (n <= 0 || vmax == 0 || w <= 1 || h <= 1) return;
    int cols = w < SCR_N ? w : SCR_N;
    const unsigned *cv = v;
    if (n != cols) {
        gfx_resample(v, n, scratch, cols, (int)vmax);
        cv = scratch;
    }
    const unsigned hi = gfx_mix(color, 0xFFFFFFu, 120);
    int prev = -1;
    for (int c = 0; c < cols; c++) {
        int hh = (int)((long)h * (long)cv[c] / (long)vmax);
        if (hh > h) hh = h;
        if (hh < 0) hh = 0;
        const int top = y + h - hh;
        const unsigned span = (unsigned)(hh > 0 ? hh : 1);
        for (int r = top; r < y + h; r++) {
            const unsigned t = (unsigned)(r - top) * 255u / span;
            gfx_blend(&g_s, x + c, r, color, 220u - t * 170u / 255u);
        }
        if (prev >= 0 && prev != top) gfx_line(&g_s, x + c - 1, prev, x + c, top, hi);
        else gfx_put(&g_s, x + c, top, hi);
        prev = top;
    }
    if (cols > 0 && n > 0) gfx_dot(&g_s, x + cols - 1, prev, 2, hi);
}

static void scale_tag(int x, int y, const char *t) {
    const int w = gfx_text_w(t) + 12;
    gfx_round(&g_s, x, y, w, 16, 5, T->bg_bot, 200);
    gfx_text(&g_s, x + 6, TXTY(y, 16), t, kFaint);
}

static void chart_area(int x, int y, int w, int h, const unsigned *v, int n, unsigned vmax, unsigned color, const char *top_tag) {
    chart_frame(x, y, w, h);
    if (n <= 0 || vmax == 0) {
        gfx_text(&g_s, x + 10, TXTY(y, h), "collecting samples", kFaint);
        return;
    }
    series_area(x, y, w, h, v, n, vmax, color);
    if (top_tag) scale_tag(x + 6, y + 6, top_tag);
}

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

static void chart_rate(int x, int y, int w, int h, const unsigned *a, const unsigned *b, unsigned ca, unsigned cb) {
    chart_frame(x, y, w, h);
    if (ring_len <= 0) return;
    const unsigned vmax = rate_scale(a, b, ring_len);
    series_area(x, y, w, h, a, ring_len, vmax, ca);
    series_area(x, y, w, h, b, ring_len, vmax, cb);
    char t[24];
    fmt_rate(t, sizeof(t), vmax);
    scale_tag(x + 6, y + 6, t);
}

static void legend_pair(int x_right, int y, const char *na, unsigned ca, const char *nb, unsigned cb) {
    const int xb = x_right - gfx_text_w(nb);
    gfx_text(&g_s, xb, TXTY(y, 18), nb, T->dim);
    gfx_dot(&g_s, xb - 10, y + 9, 3, cb);
    const int xa = xb - 22 - gfx_text_w(na);
    gfx_text(&g_s, xa, TXTY(y, 18), na, T->dim);
    gfx_dot(&g_s, xa - 10, y + 9, 3, ca);
}

static int rail_mini(void) { return RW < RAIL_FULL; }

static void header(void) {
    gfx_blend_alpha(&g_s, 0, 0, W, HDR_H, T->surface, 236);
    gfx_fill(&g_s, 0, HDR_H - 1, W, 1, T->rule);
    gfx_icon_at(&g_s, kTabIcon[tab], PAD, (HDR_H - 24) / 2);
    const int tx = PAD + 34;
    gfx_text(&g_s, tx, TXTY(0, HDR_H) - 5, kTabName[tab], T->text);
    gfx_text(&g_s, tx, TXTY(0, HDR_H) + 10, kTabHint[tab], kFaint);
    int right = W - PAD;
    char up[48], b[64];
    fmt_uptime(up, sizeof(up), g_up_s);
    put_str(b, sizeof(b), "up ");
    put_str(b + 3, (int)sizeof(b) - 3, up);
    right = pill(right, (HDR_H - 22) / 2, b, T->hover, T->dim);
    if (paused) right = pill(right, (HDR_H - 22) / 2, "PAUSED", T->danger, T->text);
    else right = pill(right, (HDR_H - 22) / 2, "LIVE", T->textfile, T->bg_bot);
}

static void rail(void) {
    const int mini = rail_mini();
    const int ry = HDR_H, rh = H - HDR_H - STAT_H;
    gfx_blend_alpha(&g_s, 0, ry, RW, rh, T->surface, 150);
    gfx_fill(&g_s, RW - 1, ry, 1, rh, T->rule);
    for (int i = 0; i < TAB_N; i++) {
        const int y = HDR_H + 12 + i * 42;
        const int on = i == tab;
        if (on) {
            gfx_panel(&g_s, 8, y, RW - 16, 36, 9, T->sel, T->accent, 255);
            gfx_round(&g_s, 8, y + 9, 3, 18, 2, T->accent_hi, 255);
        }
        const int ix = mini ? (RW - 24) / 2 : 22;
        if (!gfx_icon_at(&g_s, kTabIcon[i], ix, y + 6)) {
            gfx_dot(&g_s, ix + 12, y + 18, 5, on ? T->accent_hi : kFaint);
        }
        if (!mini) gfx_text(&g_s, 56, TXTY(y, 36), kTabName[i], on ? T->text : T->dim);
    }
    if (mini) return;
    static const char *const kKey[] = {"1-5", "Space", "C", "L", "S", "Arrows"};
    static const char *const kAct[] = {"Tabs", "Pause", "Compact", "Theme", "Sort", "Move"};
    const int nk = 6, step = 22;
    const int ny = H - STAT_H - 28 - nk * step;
    if (ny < HDR_H + 12 + TAB_N * 42 + 20) return;
    gfx_hline(&g_s, 16, ny - 12, RW - 32, T->rule);
    gfx_text(&g_s, 16, TXTY(ny - 8, 18), "Keyboard", kFaint);
    for (int i = 0; i < nk; i++) {
        const int yy = ny + 8 + i * step;
        gfx_round(&g_s, 16, yy, 58, 18, 5, T->hover, 255);
        gfx_text(&g_s, 16 + (58 - gfx_text_w(kKey[i])) / 2, TXTY(yy, 18), kKey[i], T->text);
        gfx_text(&g_s, 84, TXTY(yy, 18), kAct[i], T->dim);
    }
}

static void status_seg(int *x, int y, const char *t, unsigned color) {
    gfx_text(&g_s, *x, TXTY(y, STAT_H), t, color);
    *x += gfx_text_w(t) + 8;
    gfx_dot(&g_s, *x, y + STAT_H / 2, 1, kFaint);
    *x += 14;
}

static void status(void) {
    const int y = H - STAT_H;
    gfx_blend_alpha(&g_s, 0, y, W, STAT_H, T->surface, 240);
    gfx_fill(&g_s, 0, y, W, 1, T->rule);
    gfx_dot(&g_s, PAD + 3, y + STAT_H / 2, 3, paused ? T->warn : T->textfile);
    int x = PAD + 14;
    char b[48];
    gfx_text(&g_s, x, TXTY(y, STAT_H), paused ? "paused" : "live", T->dim);
    x += gfx_text_w(paused ? "paused" : "live") + 14;
    gfx_dot(&g_s, x, y + STAT_H / 2, 1, kFaint);
    x += 14;
    char interval[32];
    put_str(interval, sizeof(interval), "every ");
    num_str(interval + 6, interval_ms);
    int n = str_len(interval);
    put_str(interval + n, (int)sizeof(interval) - n, " ms");
    status_seg(&x, y, interval, kFaint);
    num_str(b, (unsigned long)ring_len);
    n = str_len(b);
    put_str(b + n, (int)sizeof(b) - n, " samples");
    status_seg(&x, y, b, kFaint);
    if (tab == TAB_PROC && g_nproc > 0) {
        const int last = scroll_proc + g_prows < g_nproc ? scroll_proc + g_prows : g_nproc;
        char r[48];
        put_str(r, sizeof(r), "rows ");
        num_str(r + 5, (unsigned long)scroll_proc + 1);
        n = str_len(r);
        r[n++] = '-';
        num_str(r + n, (unsigned long)last);
        n = str_len(r);
        put_str(r + n, (int)sizeof(r) - n, " of ");
        num_str(r + n + 4, (unsigned long)g_nproc);
        status_seg(&x, y, r, kFaint);
    } else {
        char t[48];
        if (g_nproc_total > g_nproc) {
            num_str(t, (unsigned long)g_nproc);
            n = str_len(t);
            put_str(t + n, (int)sizeof(t) - n, " of ");
            n = str_len(t);
            num_str(t + n, (unsigned long)g_nproc_total);
        } else {
            num_str(t, (unsigned long)g_nproc_total);
        }
        n = str_len(t);
        put_str(t + n, (int)sizeof(t) - n, " tasks");
        status_seg(&x, y, t, kFaint);
    }
    const char *nic = g_net_ready ? g_iface : "no nic";
    const int rw = gfx_text_w(nic);
    gfx_text_right(&g_s, W - PAD, TXTY(y, STAT_H), nic, kFaint);
    gfx_dot(&g_s, W - PAD - rw - 12, y + STAT_H / 2, 3, g_net_ready ? T->rx : kFaint);
}

static void tab_overview(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    const int colw = (cw - GAP) / 2;
    const int x2 = x0 + colw + GAP;
    const int bottom = H - STAT_H - PAD;
    const int h1 = 164, h2 = 150;
    int y = HDR_H + PAD;
    char b[48], c[48];

    card(x0, y, colw, h1);
    fmt_pct(b, last_of(h_cpu));
    char mhz[24];
    mhz[0] = 0;
    if (g_mhz) { num_str(mhz, g_mhz); put_str(mhz + str_len(mhz), (int)sizeof(mhz) - str_len(mhz), " MHz"); }
    card_head(x0, y, colw, "Processor", T->cpu, g_mhz ? mhz : 0);
    gfx_text(&g_s, x0 + 16, TXTY(y + 44, 18), b, T->text);
    put_str(c, sizeof(c), "ctx ");
    num_sep(c + 4, (int)sizeof(c) - 4, ctxt_rate);
    gfx_text_right(&g_s, x0 + colw - 14, TXTY(y + 44, 18), c, kFaint);
    chart_area(x0 + 14, y + 72, colw - 28, h1 - 72 - 14, h_cpu, ring_len, 100, T->cpu, "100%");

    card(x2, y, colw, h1);
    char tot[24], used[24];
    num_sep(tot, sizeof(tot), g_mem.total_kb / 1024);
    num_sep(used, sizeof(used), (g_mem.total_kb >= g_mem.free_kb ? g_mem.total_kb - g_mem.free_kb : 0) / 1024);
    char tm[40];
    put_str(tm, sizeof(tm), tot);
    put_str(tm + str_len(tm), (int)sizeof(tm) - str_len(tm), " MiB");
    card_head(x2, y, colw, "Memory", T->mem, tm);
    char ut[48];
    put_str(ut, sizeof(ut), used);
    put_str(ut + str_len(ut), (int)sizeof(ut) - str_len(ut), " MiB in use");
    gfx_text(&g_s, x2 + 16, TXTY(y + 44, 18), ut, T->text);
    fmt_pct(b, last_of(h_mem));
    gfx_text_right(&g_s, x2 + colw - 14, TXTY(y + 44, 18), b, T->mem);
    meter(x2 + 14, y + 66, colw - 28, (int)last_of(h_mem), T->mem);
    chart_area(x2 + 14, y + 86, colw - 28, h1 - 86 - 14, h_mem, ring_len, 100, T->mem, "100%");
    y += h1 + GAP;

    const int hw = (colw - 28) / 2;
    card(x0, y, colw, h2);
    card_head(x0, y, colw, "Storage", T->disk, g_disk.present ? "ATA" : "FAT32 ramdisk");
    if (!g_disk.present) {
        gfx_text(&g_s, x0 + 16, TXTY(y + 46, 18), "no ATA device attached", kFaint);
        gfx_text(&g_s, x0 + 16, TXTY(y + 70, 18), "volume is the FAT32 ramdisk", kFaint);
    } else {
        char r[32], w[32];
        fmt_rate(r, sizeof(r), rd_rate);
        fmt_rate(w, sizeof(w), wr_rate);
        kv_stack(x0 + 16, y + 46, hw, "read", r, T->disk);
        kv_stack(x0 + 16, y + 84, hw, "write", w, kWrite);
    }
    chart_rate(x0 + 14 + hw + 14, y + 46, colw - hw - 42, h2 - 46 - 14, h_rd, h_wr, T->disk, kWrite);

    card(x2, y, colw, h2);
    card_head(x2, y, colw, "Network", T->rx, g_net.present ? g_iface : "no nic");
    if (!g_net.present) {
        gfx_text(&g_s, x2 + 16, TXTY(y + 46, 18), "no NIC at boot", kFaint);
        gfx_text(&g_s, x2 + 16, TXTY(y + 70, 18), "run QEMU with -nic", kFaint);
    } else {
        char r[32], t[32];
        fmt_rate(r, sizeof(r), rx_rate);
        fmt_rate(t, sizeof(t), tx_rate);
        kv_stack(x2 + 16, y + 46, hw, "down", r, T->rx);
        kv_stack(x2 + 16, y + 84, hw, "up", t, T->tx);
    }
    chart_rate(x2 + 14 + hw + 14, y + 46, colw - hw - 42, h2 - 46 - 14, h_rx, h_tx, T->rx, T->tx);
    y += h2 + GAP;

    const int hs = bottom - y;
    if (hs >= 80) {
        card(x0, y, cw, hs);
        card_head(x0, y, cw, "System", T->proc, 0);
        const int cwid = (cw - 28) / 3;
        int ty = y + HEAD_H + 12;
        gfx_text(&g_s, x0 + 16, TXTY(ty, 18), g_model, T->text);
        ty += 30;
        char v[40];
        put_str(v, sizeof(v), g_vendor);
        kv_stack(x0 + 16, ty, cwid, "Vendor", v, T->text);
        num_str(v, g_family);
        kv_stack(x0 + 16 + cwid, ty, cwid, "Family", v, T->text);
        num_str(v, g_modno);
        kv_stack(x0 + 16 + cwid * 2, ty, cwid, "Model", v, T->text);
        ty += 40;
        num_str(v, g_step);
        kv_stack(x0 + 16, ty, cwid, "Stepping", v, T->text);
        char disp[24];
        num_str(disp, g_fb_w);
        put_str(disp + str_len(disp), (int)sizeof(disp) - str_len(disp), " x ");
        num_str(disp + str_len(disp), g_fb_h);
        kv_stack(x0 + 16 + cwid, ty, cwid, "Display", disp, T->text);
        kv_stack(x0 + 16 + cwid * 2, ty, cwid, "Features", g_feat[0] ? g_feat : "none reported", T->text);
    }
}

static void tab_perf(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    int y = HDR_H + PAD;
    const int ch = 232;
    card(x0, y, cw, ch);
    char b[24];
    fmt_pct(b, last_of(h_cpu));
    card_head(x0, y, cw, "CPU history", T->cpu, b);
    chart_area(x0 + 14, y + HEAD_H + 10, cw - 80, ch - HEAD_H - 24, h_cpu, ring_len, 100, T->cpu, "100%");
    const int sx = x0 + cw - 58;
    gfx_text(&g_s, sx, TXTY(y + HEAD_H + 10, 18), "100", kFaint);
    gfx_text(&g_s, sx, TXTY(y + HEAD_H + 10 + (ch - HEAD_H - 24) / 2 - 9, 18), "50", kFaint);
    gfx_text(&g_s, sx, TXTY(y + ch - 24, 18), "0", kFaint);
    y += ch + GAP;

    const int bottom = H - STAT_H - PAD;
    card(x0, y, cw, bottom - y);
    card_head(x0, y, cw, "System counters", T->proc, 0);
    const int c2 = (cw - 28 - GAP) / 2;
    const int lx = x0 + 16, rx = lx + c2 + GAP;
    char v[32];
    int yy = y + HEAD_H + 12;
    num_sep(v, sizeof(v), g_cpu.user);
    kv_row(lx, yy, c2, "user jiffies", v);
    num_sep(v, sizeof(v), g_cpu.system);
    kv_row(rx, yy, c2, "system jiffies", v);
    yy += 24;
    num_sep(v, sizeof(v), g_cpu.idle);
    kv_row(lx, yy, c2, "idle jiffies", v);
    num_sep(v, sizeof(v), ctxt_rate);
    kv_row(rx, yy, c2, "context switches/s", v);
    yy += 36;
    gfx_text(&g_s, lx, TXTY(yy, 18), "CPU time by process (lifetime)", T->dim);
    yy += 24;
    unsigned long maxj = 1;
    for (int i = 0; i < g_nproc; i++)
        if (g_procs[i].jiffy > maxj) maxj = g_procs[i].jiffy;
    const int barx = x0 + 150, barw = cw - 150 - 110;
    for (int i = 0; i < g_nproc && yy + ROW_H < bottom; i++) {
        gfx_text(&g_s, lx, TXTY(yy, 18), g_procs[i].name, T->text);
        const int fill = (int)(g_procs[i].jiffy * (unsigned long)barw / maxj);
        gfx_round(&g_s, barx, yy + 6, barw, 8, 4, T->hover, 255);
        if (fill > 0) gfx_round(&g_s, barx, yy + 6, fill < 8 ? 8 : fill, 8, 4, T->cpu, 255);
        char nb[24];
        num_sep(nb, sizeof(nb), g_procs[i].jiffy);
        gfx_text_right(&g_s, x0 + cw - 14, TXTY(yy, 18), nb, T->dim);
        yy += ROW_H;
    }
}

typedef struct { int pid_r, name_x, name_end, state_x, cpu_r, jif_r, mem_r, show_jif, show_state; } ProcCols;
static ProcCols pc;

static void proc_cols(int x0, int cw) {
    pc.mem_r = x0 + cw - 16;
    pc.show_jif = cw >= 460;
    pc.show_state = cw >= 360;
    pc.jif_r = pc.mem_r - gfx_text_w("99,999 KiB") - 18;
    const int left = pc.show_jif ? pc.jif_r - gfx_text_w("999,999") : pc.mem_r - gfx_text_w("99,999 KiB");
    pc.cpu_r = left - 18;
    const int cpu_l = pc.cpu_r - gfx_text_w("CPU %");
    pc.state_x = cpu_l - 20 - gfx_text_w("State");
    pc.name_end = (pc.show_state ? pc.state_x : cpu_l) - 12;
    pc.pid_r = x0 + 14 + gfx_text_w("9999");
    pc.name_x = pc.pid_r + 18;
}

static int proc_head_hit(int x) {
    if (x >= pc.mem_r - gfx_text_w("99,999 KiB") - 8) return SORT_MEM;
    if (x >= pc.cpu_r - gfx_text_w("CPU %") - 16 && x <= pc.cpu_r + 10) return SORT_CPU;
    if (x < pc.name_x - 4) return SORT_PID;
    return -1;
}

static void sort_arrow(int x, int y, int desc) {
    for (int r = 0; r < 4; r++) {
        const int w = desc ? 7 - 2 * r : 1 + 2 * r;
        gfx_hline(&g_s, x + (7 - w) / 2, y + r, w, T->accent_hi);
    }
}

static void proc_head(int y) {
    const unsigned cp = sort_key == SORT_PID ? T->accent_hi : kFaint;
    const unsigned cc = sort_key == SORT_CPU ? T->accent_hi : kFaint;
    const unsigned cm = sort_key == SORT_MEM ? T->accent_hi : kFaint;
    gfx_text_right(&g_s, pc.pid_r, y, "PID", cp);
    if (sort_key == SORT_PID) sort_arrow(pc.pid_r - gfx_text_w("PID") - 12, y + 7, sort_desc);
    gfx_text(&g_s, pc.name_x, y, "Process", kFaint);
    if (pc.show_state) gfx_text(&g_s, pc.state_x, y, "State", kFaint);
    gfx_text_right(&g_s, pc.cpu_r, y, "CPU %", cc);
    if (sort_key == SORT_CPU) sort_arrow(pc.cpu_r - gfx_text_w("CPU %") - 12, y + 7, sort_desc);
    if (pc.show_jif) gfx_text_right(&g_s, pc.jif_r, y, "Jiffies", kFaint);
    gfx_text_right(&g_s, pc.mem_r, y, "Memory", cm);
    if (sort_key == SORT_MEM) sort_arrow(pc.mem_r - gfx_text_w("Memory") - 12, y + 7, sort_desc);
}

static void tab_proc(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    const int top = HDR_H + PAD;
    const int bottom = H - STAT_H - PAD;
    proc_cols(x0, cw);
    card(x0, top, cw, bottom - top);
    card_head(x0, top, cw, "Processes", T->mem, 0);
    proc_head(top + HEAD_H + 8);
    const int first = top + HEAD_H + 30;
    int rows = (bottom - first - 6) / ROW_H;
    if (rows < 1) rows = 1;
    g_prows = rows;
    if (sel_proc >= g_nproc) sel_proc = g_nproc ? g_nproc - 1 : 0;
    if (sel_proc < scroll_proc) scroll_proc = sel_proc;
    if (sel_proc >= scroll_proc + rows) scroll_proc = sel_proc - rows + 1;
    if (scroll_proc > g_nproc - rows) scroll_proc = g_nproc - rows;
    if (scroll_proc < 0) scroll_proc = 0;
    const int name_avail = pc.name_end - pc.name_x;
    for (int i = scroll_proc; i < g_nproc && i < scroll_proc + rows; i++) {
        const int ry = first + (i - scroll_proc) * ROW_H;
        const Proc *p = &g_procs[g_order[i]];
        if (i == sel_proc) {
            gfx_round(&g_s, x0 + 8, ry - 3, cw - 16, ROW_H - 2, R_ROW, T->sel, 255);
            gfx_round(&g_s, x0 + 8, ry - 3, 3, ROW_H - 2, 1, T->accent_hi, 255);
        } else if ((i & 1) == 0) {
            gfx_round(&g_s, x0 + 8, ry - 3, cw - 16, ROW_H - 2, R_ROW, T->hover, 90);
        }
        char b[32];
        num_str(b, p->pid);
        gfx_text_right(&g_s, pc.pid_r, TXTY(ry, 18), b, kFaint);
        if (name_avail >= 8) {
            char nm[24];
            put_str(nm, sizeof(nm), p->name[0] ? p->name : "-");
            int nl = str_len(nm);
            if (gfx_text_w(nm) > name_avail) {
                while (nl > 3 && gfx_text_w(nm) > name_avail) nm[--nl] = 0;
                if (nl > 3) { nm[nl - 1] = '.'; nm[nl - 2] = '.'; }
            }
            gfx_text(&g_s, pc.name_x, TXTY(ry, 18), nm, T->text);
        }
        if (pc.show_state) {
            const unsigned base = p->state == 'Z' ? T->danger : p->state == 'R' ? T->cpu : T->dim;
            gfx_round(&g_s, pc.state_x - 2, ry - 1, 16, 18, 5, gfx_mix(base, 0x000000u, 150), 255);
            char st[2] = {p->state ? p->state : 'R', 0};
            gfx_text(&g_s, pc.state_x + 3, TXTY(ry, 18), st, T->text);
        }
        num_str(b, (unsigned long)p->cpu);
        const int k = str_len(b);
        b[k] = '%';
        b[k + 1] = 0;
        gfx_text_right(&g_s, pc.cpu_r, TXTY(ry, 18), b, p->cpu ? T->cpu : kFaint);
        if (pc.show_jif) {
            num_sep(b, sizeof(b), p->jiffy);
            gfx_text_right(&g_s, pc.jif_r, TXTY(ry, 18), b, T->dim);
        }
        char mb[32];
        num_sep(mb, sizeof(mb), p->mem / 1024);
        put_str(mb + str_len(mb), (int)sizeof(mb) - str_len(mb), " KiB");
        gfx_text_right(&g_s, pc.mem_r, TXTY(ry, 18), mb, T->dim);
    }
    if (!g_nproc) gfx_text(&g_s, x0 + 16, first, "no processes", kFaint);
    gfx_scrollbar(&g_s, x0 + cw - 8, first, bottom - first - 8, g_nproc, rows, scroll_proc, T->surface, T->accent);
}

static void tab_disk(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    const int colw = (cw - GAP) / 2;
    const int x2 = x0 + colw + GAP;
    const int bottom = H - STAT_H - PAD;
    int y = HDR_H + PAD;
    const int h1 = 176;
    char b[32];

    card(x0, y, colw, h1);
    card_head(x0, y, colw, "FAT32 volume", T->disk, g_fat_state);
    if (g_fat_total == 0) {
        gfx_text(&g_s, x0 + 16, TXTY(y + 52, 18), "not mounted", kFaint);
    } else {
        char used[24], tot[24];
        num_sep(used, sizeof(used), (g_fat_total - g_fat_free) / 1048576);
        num_sep(tot, sizeof(tot), g_fat_total / 1048576);
        const int pct = (int)((g_fat_total - g_fat_free) * 100 / g_fat_total);
        meter_row(x0 + 16, y + 46, colw - 32, "Capacity", tot, pct, T->disk);
        put_str(b, sizeof(b), used);
        put_str(b + str_len(b), (int)sizeof(b) - str_len(b), " MiB used");
        gfx_text(&g_s, x0 + 16, TXTY(y + 98, 18), b, T->text);
        num_str(b, g_fat_entries);
        kv_stack(x0 + 16, y + 126, colw / 2 - 16, "Entries", b, T->text);
        kv_stack(x0 + 16 + colw / 2, y + 126, colw / 2 - 16, "Free MiB", tot, T->text);
    }

    card(x2, y, colw, h1);
    card_head(x2, y, colw, "ATA device", T->disk, 0);
    if (g_disk.present) legend_pair(x2 + colw - 14, y + 8, "read", T->disk, "write", kWrite);
    if (!g_disk.present) {
        gfx_text(&g_s, x2 + 16, TXTY(y + 52, 18), "no device on the IDE channels", kFaint);
        gfx_text(&g_s, x2 + 16, TXTY(y + 76, 18), "counters stay empty", kFaint);
    } else {
        const int hw = (colw - 32) / 2;
        char v[32];
        num_str(v, g_disk.reads);
        kv_stack(x2 + 16, y + 46, hw, "read transfers", v, T->text);
        num_str(v, g_disk.rsect);
        kv_stack(x2 + 16 + hw, y + 46, hw, "sectors read", v, T->text);
        num_str(v, g_disk.writes);
        kv_stack(x2 + 16, y + 92, hw, "write transfers", v, T->text);
        num_str(v, g_disk.wsect);
        kv_stack(x2 + 16 + hw, y + 92, hw, "sectors written", v, T->text);
    }
    y += h1 + GAP;

    card(x0, y, cw, bottom - y);
    char r[32], w[32];
    fmt_rate(r, sizeof(r), rd_rate);
    fmt_rate(w, sizeof(w), wr_rate);
    card_head(x0, y, cw, "Throughput and RAM overlay", kWrite, 0);
    legend_pair(x0 + cw - 14, y + 8, "read", T->disk, "write", kWrite);
    const int kw = (cw - 28) / 4;
    kv_stack(x0 + 16, y + HEAD_H + 10, kw, "read", r, T->disk);
    kv_stack(x0 + 16 + kw, y + HEAD_H + 10, kw, "write", w, kWrite);
    char v[32], sz[32];
    num_str(v, g_wfiles);
    kv_stack(x0 + 16 + kw * 2, y + HEAD_H + 10, kw, "writable files", v, T->text);
    gfx_fmt_size(sz, g_wbytes);
    kv_stack(x0 + 16 + kw * 3, y + HEAD_H + 10, kw, "writable bytes", sz, T->text);
    const int chh = bottom - (y + HEAD_H + 52) - 14;
    if (chh >= 24) chart_rate(x0 + 14, y + HEAD_H + 52, cw - 28, chh, h_rd, h_wr, T->disk, kWrite);
}

static void tab_net(void) {
    const int x0 = RW + PAD;
    const int cw = W - x0 - PAD;
    const int bottom = H - STAT_H - PAD;
    const int lcol = x0 + 16;
    int y = HDR_H + PAD;

    const int ha = 92;
    card(x0, y, cw, ha);
    card_head(x0, y, cw, "Network adapter", T->rx, g_net_ready ? g_driver : "down");
    const int aw = (cw - 32) / 3;
    if (!g_net_ready) {
        gfx_text(&g_s, lcol, TXTY(y + 46, 18), "No NIC was claimed at boot.", T->dim);
        gfx_text(&g_s, lcol, TXTY(y + 68, 18), "Run QEMU with -nic user,model=virtio", kFaint);
    } else {
        kv_stack(lcol, y + 46, aw, "address", g_ip, T->text);
        kv_stack(lcol + aw, y + 46, aw, "gateway", g_gw, T->text);
        kv_stack(lcol + aw * 2, y + 46, aw, "dns", g_dns, T->text);
    }
    y += ha + GAP;

    const int ht = 150;
    card(x0, y, cw, ht);
    card_head(x0, y, cw, "Traffic", T->rx, 0);
    legend_pair(x0 + cw - 14, y + 8, "down", T->rx, "up", T->tx);
    char r[32], t[32], rf[32], tf[32];
    fmt_rate(r, sizeof(r), rx_rate);
    fmt_rate(t, sizeof(t), tx_rate);
    num_str(rf, g_net.rx_p);
    num_str(tf, g_net.tx_p);
    const int kw = (cw - 32) / 4;
    kv_stack(lcol, y + 46, kw, "down", r, T->rx);
    kv_stack(lcol + kw, y + 46, kw, "up", t, T->tx);
    kv_stack(lcol + kw * 2, y + 46, kw, "frames rx", rf, T->text);
    kv_stack(lcol + kw * 3, y + 46, kw, "frames tx", tf, T->text);
    const int chh = ht - 84 - 12;
    chart_rate(lcol, y + 84, cw - 32, chh, h_rx, h_tx, T->rx, T->tx);
    y += ht + GAP;

    card(x0, y, cw, bottom - y);
    card_head(x0, y, cw, "Connections", T->mem, 0);
    int yy = y + HEAD_H + 10;
    gfx_round(&g_s, x0 + 8, yy - 2, cw - 16, 20, 5, T->hover, 110);
    gfx_text(&g_s, x0 + 18, TXTY(yy - 2, 20), "proto", kFaint);
    gfx_text(&g_s, x0 + 78, TXTY(yy - 2, 20), "local", kFaint);
    gfx_text(&g_s, x0 + 262, TXTY(yy - 2, 20), "remote", kFaint);
    gfx_text_right(&g_s, x0 + cw - 18, TXTY(yy - 2, 20), "state", kFaint);
    yy += 26;
    if (!g_nconn) gfx_text(&g_s, x0 + 18, TXTY(yy, 18), "no open connections", kFaint);
    for (int i = scroll_net; i < g_nconn && yy + ROW_H < bottom - 6; i++) {
        const Conn *c = &g_conn[i];
        char la[32], ra[32];
        ip_str(c->lip, la);
        int p = str_len(la);
        la[p++] = ':';
        num_str(la + p, c->lport);
        ip_str(c->rip, ra);
        p = str_len(ra);
        ra[p++] = ':';
        num_str(ra + p, c->rport);
        if ((i & 1) == 0) gfx_round(&g_s, x0 + 8, yy - 3, cw - 16, ROW_H - 2, R_ROW, T->hover, 60);
        gfx_text(&g_s, x0 + 18, TXTY(yy, 18), c->udp ? "udp" : "tcp", kFaint);
        gfx_text(&g_s, x0 + 78, TXTY(yy, 18), la, T->text);
        gfx_text(&g_s, x0 + 262, TXTY(yy, 18), ra, T->text);
        gfx_text_right(&g_s, x0 + cw - 18, TXTY(yy, 18), c->state,
                       c->state[0] == 'E' ? T->textfile : T->dim);
        yy += ROW_H;
    }
    if (g_narp && yy + 40 < bottom) {
        yy += 6;
        gfx_hline(&g_s, x0 + 16, yy, cw - 32, T->rule);
        yy += 10;
        gfx_text(&g_s, x0 + 18, TXTY(yy, 18), "ARP neighbours", T->dim);
        yy += 24;
        for (int i = 0; i < g_narp && yy + ROW_H < bottom - 6; i++) {
            gfx_text(&g_s, x0 + 18, TXTY(yy, 18), g_arp[i].ip, T->text);
            gfx_text(&g_s, x0 + 262, TXTY(yy, 18), g_arp[i].mac, T->dim);
            yy += ROW_H;
        }
    }
    gfx_scrollbar(&g_s, x0 + cw - 9, y + HEAD_H + 36, bottom - y - HEAD_H - 46, g_nconn, 8, scroll_net, T->surface, T->accent);
}

static void render_compact(void) {
    char b[24];
    card(PAD, PAD, W - 2 * PAD, H - 2 * PAD);
    fmt_pct(b, last_of(h_cpu));
    gfx_dot(&g_s, PAD + 16, PAD + 17, 3, T->cpu);
    gfx_text(&g_s, PAD + 26, TXTY(PAD + 8, 18), "CPU", T->text);
    gfx_text_right(&g_s, W - PAD - 14, TXTY(PAD + 8, 18), b, T->cpu);
    meter(PAD + 16, PAD + 40, W - 2 * PAD - 32, (int)last_of(h_cpu), T->cpu);
    fmt_pct(b, last_of(h_mem));
    gfx_dot(&g_s, PAD + 16, PAD + 67, 3, T->mem);
    gfx_text(&g_s, PAD + 26, TXTY(PAD + 58, 18), "Memory", T->text);
    gfx_text_right(&g_s, W - PAD - 14, TXTY(PAD + 58, 18), b, T->mem);
    meter(PAD + 16, PAD + 82, W - 2 * PAD - 32, (int)last_of(h_mem), T->mem);
    const int cy = PAD + 104;
    chart_area(PAD + 14, cy, W - 2 * PAD - 28, H - 2 * PAD - (cy - PAD) - 14, h_cpu, ring_len, 100, T->cpu, 0);
}

static void render(void) {
    bg();
    if (compact) {
        render_compact();
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

static void apply_size(int w, int h) {
    if (w < 200) w = 200;
    if (h < 150) h = 150;
    if (w > CAP_W) w = CAP_W;
    if (h > CAP_H) h = CAP_H;
    W = w;
    H = h;
    RW = W < RAIL_COLLAPSE_W ? RAIL_MINI : RAIL_FULL;
    if (!compact) { full_w = W; full_h = H; }
    g_s.w = W;
    g_s.h = H;
    g_s.pitch = (int)g_win->pitch;
    if (g_s.pitch < W) g_s.pitch = W;
    dirty = 1;
}

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
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c == 'q') quit_now = 1;
    else if (c >= '1' && c <= '5') { tab = c - '1'; scroll_net = 0; }
    else if (c == '\t') tab = (tab + 1) % TAB_N;
    else if (c == ' ') paused = !paused;
    else if (c == 'c') {
        compact = !compact;
        const int nw = compact ? 340 : full_w, nh = compact ? 230 : full_h;
        const long r = choco_win_resize(g_addr, nw, nh);
        if (r < 0) {
            choco_print("sysmon: resize refused\n");
            compact = !compact;
        } else {
            apply_size(nw, nh);
        }
    } else if (c == 'l') {
        T = gui_theme_toggle(T);
    } else if (c == 'r') {
        reset_history();
    } else if (c == '+' || c == '=') {
        if (interval_ms < 2000) interval_ms += 250;
    } else if (c == '-' && interval_ms > 250) {
        interval_ms -= 250;
    } else if (c == 'j' || c == 2 || c == 10) {
        nav(K_DOWN);
    } else if (c == 'k' || c == 8) {
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

static int esc_state = 0;
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
        esc_state = 0;
    } else if (esc_state == 2) {
        if (info >= '0' && info <= '9') {
            if (!esc_semi) esc_param = esc_param * 10 + (info - '0');
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
#ifdef KEY_UP
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
    if (!compact && x < RW && y >= HDR_H + 12) {
        const int rel = y - (HDR_H + 12);
        const int i = rel / 42;
        if (i >= 0 && i < TAB_N && rel % 42 < 36) {
            tab = i;
            scroll_net = 0;
        }
        return;
    }
    if (!compact && tab == TAB_PROC) {
        const int x0 = RW + PAD;
        proc_cols(x0, W - x0 - PAD);
        const int top = HDR_H + PAD;
        const int first = top + HEAD_H + 30;
        if (y >= top + HEAD_H + 6 && y < top + HEAD_H + 26) {
            const int k = proc_head_hit(x);
            if (k >= 0) set_sort(k);
            return;
        }
        if (y >= first) {
            const int row = (y - first) / ROW_H + scroll_proc;
            if (row >= 0 && row < g_nproc) sel_set(row);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const long addr = choco_win_create(W, H, "System Monitor");
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
    for (;;) {
        while (g_win->tail != g_win->head) {
            struct uw_ev e = g_win->ev[g_win->tail % 32];
            g_win->tail++;
            if (e.type == UEV_KEY) on_key((unsigned)e.info);
            else if (e.type == UEV_MOUSE) handle_click((int)e.x, (int)e.y);
            else if (e.type == UEV_RESIZE) apply_size((int)e.x, (int)e.y);
        }
        const unsigned long t = (unsigned long)choco_ticks() * 10;
        if (esc_state == 1 && t - esc_t >= 60) quit_now = 1;
        else if (esc_state == 2 && t - esc_t >= 500) esc_state = 0;
        if (quit_now) {
            choco_win_close(addr);
            return 0;
        }
        if (!paused && t >= next) {
            sample();
            next = t + interval_ms;
            dirty = 1;
        }
        if (dirty || t - last_draw >= 1000) {
            render();
            choco_win_present(addr);
            dirty = 0;
            last_draw = t;
        }
        choco_sleep(40);
    }
}

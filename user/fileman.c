// /bin/fileman - ChocoOS File Explorer (ring 3, display server client).
// Fluent/Windows-11 styled shell over the real filesystem tree (initrd +
// /disk FAT32 + /proc + the writable ram fs). Painted into the window's shared
// pixel buffer with the primitives in user/lib/gui.h.
#include "lib.h"
#include "lib/gui.h"
#include "lib/fs_client.h"

// Window size is variable: the WM can resize us and reports it via UEV_RESIZE.
// The shared buffer is allocated at capacity (640x480, stride 640), so the
// pixel pointer stays valid and only the visible extent changes.
static int W = 620;
static int H = 460;
#define TOOLBAR_H 40       // navigation row; native title bar is owned by the WM
#define COMMAND_H 38       // concise command row, similar to Windows Explorer
#define CONTENT_TOP (TOOLBAR_H + COMMAND_H)
#define SIDEBAR_W 144
#define STATUS_H 24
#define ROW_H 30
#define HDR_H 25
#define VIEW_LINE (UIFONT_LINE + 1)
#define NAV_N 16

static struct uw_win *g_win;
static GuiSurface g_s;
static long g_addr = 0;
static int compact = 0;   // 'f' toggles a small window to test/appreciate resize

static char cwd[FS_NAME_CAP];                 // "" (root) or "a/b/"
static struct FsEnt ents[FS_MAX_ENT];
static unsigned char kinds[FS_MAX_ENT];       // 0 folder, 1 text, 2 binary (ELF)
static int nent = 0, sel = 0, scroll = 0;

static char hist[NAV_N][FS_NAME_CAP];
static int hcount = 0, hpos = 0;

static int viewing = 0;
static char vpath[FS_NAME_CAP];
static char vbuf[8192];
static long vlen = 0;
static int vscroll = 0;

static int g_quit = 0;
static char g_msg[48];
static long g_msg_when = 0;

static int g_prompt = 0; // 0 none, 1 new file, 2 new folder, 3 rename, 4 delete confirm
static char g_input[64];
static int g_ilen = 0;
/* File Explorer uses a restrained neutral Fluent palette. Other apps keep the
   shared Midnight Amethyst theme; this app still offers matching light/dark modes. */
static const GuiTheme kExplorerDark = {
    0x1B1B1B, 0x1B1B1B, 0x191919, 0x202020, 0x2A2A2A, 0x353535, 0x454545, 0x303030,
    0xF3F3F3, 0xD0D0D0, 0x9D9D9D, 0x60CDFF, 0x92DEFF, 0x0078D4,
    0xE7A23D, 0x93CDB0, 0x77B7F2, 0x202020, 0x101010,
    0x6FA8DC, 0x55B7D9, 0x6EBE8A, 0xE6B655, 0xD880A6, 0xB99BE8, 0xE05D5D, 0xE05D5D, 0x303030, 0x7CFC00
};
static const GuiTheme kExplorerLight = {
    0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xF5F5F5, 0xE8E8E8, 0xE4F2FB, 0xD0D0D0, 0xE2E2E2,
    0x1B1B1B, 0x343434, 0x686868, 0x0067C0, 0x005A9E, 0x004578,
    0xB87516, 0x317C55, 0x1769AA, 0xF5F5F5, 0xFFFFFF,
    0x6FA8DC, 0x55B7D9, 0x6EBE8A, 0xC68A20, 0xB85A82, 0x8C6BC4, 0xD83B3B, 0xD83B3B, 0xE2E2E2, 0x4D7C0F
};
static const GuiTheme *g_theme = &kExplorerDark;

static int g_last_row = -1;
static long g_last_tick = 0;

// ---------- adaptive geometry ------------------------------------------------
static int side_w(void) { return W < 500 ? 120 : (W < 570 ? 132 : SIDEBAR_W); }
static int list_card_x(void) { return side_w() + 1; }
static int list_card_y(void) { return CONTENT_TOP; }
static int list_card_w(void) { return W - list_card_x(); }
static int list_card_h(void) { return H - STATUS_H - list_card_y(); }
static int list_y0(void) { return list_card_y() + HDR_H; }
static int list_y1(void) { return H - STATUS_H; }
static int rows_visible(void) {
    int area = list_y1() - list_y0();
    return area > 0 ? area / ROW_H : 0;
}

static void set_msg(const char *m) {
    fs_strcpy(g_msg, m, sizeof(g_msg));
    g_msg_when = choco_ticks();
}

// Navigation buttons: 0 back, 1 forward, 2 up, 3 refresh.
static void nav_rect(int i, int *x, int *y, int *w, int *h) {
    int gap = W < 520 ? 32 : 34;
    int size = W < 520 ? 28 : 30;
    *x = 9 + i * gap;
    *y = 5;
    *w = size;
    *h = 28;
}
static void addr_rect(int *x, int *y, int *w, int *h) {
    int bx, by, bw, bh;
    nav_rect(3, &bx, &by, &bw, &bh);
    *x = bx + bw + 9;
    *y = by;
    *w = W - *x - (W >= 360 ? 48 : 10);
    *h = bh;
    if (*w < 36) *w = 36;
}
static void quick_rect(int i, int *x, int *y, int *w, int *h) {
    *x = 8;
    *y = CONTENT_TOP + 10 + i * 36;
    *w = side_w() - 16;
    *h = 32;
}
static const char *kQuickName[] = {"Home", "System", "Disk", "Processes"};
static const char *kQuickPath[] = {"", "bin/", "disk/", "proc/"};
static const int kQuickN = 4;

// Command-bar actions: 0 create file, 1 create folder, 2 rename, 3 delete.
static void action_rect(int i, int *x, int *y, int *w, int *h) {
    static const int wide_w[4] = {76, 90, 72, 60};
    static const int compact_w[4] = {70, 74, 68, 58};
    static const int narrow_w[4] = {42, 50, 38, 38};
    static const int tiny_w[4] = {40, 34, 34, 34};
    const int *widths = W >= 560 ? wide_w : (W >= 440 ? compact_w : (W >= 360 ? narrow_w : tiny_w));
    int gap = W >= 560 ? 6 : (W >= 360 ? 4 : 2);
    int bx = W >= 360 ? 12 : 6;
    for (int k = 0; k < i; k++) bx += widths[k] + gap;
    *x = bx;
    *y = TOOLBAR_H + 5;
    *w = widths[i];
    *h = 28;
}
static const char *action_label(int i) {
    if (W >= 560) {
        static const char *labels[] = {"New file", "New folder", "Rename", "Delete"};
        return labels[i];
    }
    if (W >= 440) {
        static const char *labels[] = {"New file", "+ Folder", "Rename", "Delete"};
        return labels[i];
    }
    if (W >= 360) {
        static const char *labels[] = {"File", "Folder", "Ren", "Del"};
        return labels[i];
    }
    {
        static const char *labels[] = {"File", "Dir", "Ren", "Del"};
        return labels[i];
    }
}

static const GuiTheme *toggle_explorer_theme(const GuiTheme *current) {
    return current == &kExplorerLight ? &kExplorerDark : &kExplorerLight;
}

static void prompt_rect(int *bx, int *by) {
    int pw = W - 24;
    int ph = H - 24;
    if (pw > 360) pw = 360;
    if (ph > 156) ph = 156;
    *bx = (W - pw) / 2;
    *by = (H - ph) / 2;
}
static int prompt_width(void) { int v = W - 24; return v > 360 ? 360 : v; }
static int prompt_height(void) { int v = H - 24; return v > 156 ? 156 : v; }

// ---------- state ----------
// ELF magic decides "binary"; everything else the shell can show as text.
static int probe_kind(const char *path) {
    char b[4];
    long fd = choco_open(path);
    if (fd < 0) return 1;
    long n = choco_read(fd, b, 4);
    choco_close(fd);
    if (n >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') return 2;
    return 1;
}

static void refresh(void) {
    nent = fs_list_dir(cwd, ents, FS_MAX_ENT);
    fs_sort(ents, nent);
    for (int i = 0; i < nent; i++) {
        if (ents[i].is_dir) { kinds[i] = 0; continue; }
        // Type comes from the file itself: extensionless procfs files are text.
        char path[256];
        fs_join(cwd, ents[i].name, 0, path, sizeof(path));
        kinds[i] = probe_kind(path);
    }
    if (sel >= nent) sel = nent ? nent - 1 : 0;
    if (sel < 0) sel = 0;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + rows_visible()) scroll = sel - rows_visible() + 1;
    if (scroll < 0) scroll = 0;
}

static void push_history(const char *dir) {
    if (hcount > 0 && fs_streq(hist[hcount - 1], dir)) return;
    if (hcount < NAV_N) {
        fs_strcpy(hist[hcount], dir, FS_NAME_CAP);
        hpos = hcount;
        hcount++;
    } else {
        for (int i = 0; i + 1 < NAV_N; i++) fs_strcpy(hist[i], hist[i + 1], FS_NAME_CAP);
        fs_strcpy(hist[NAV_N - 1], dir, FS_NAME_CAP);
        hpos = NAV_N - 1;
    }
}

static void go_dir(const char *dir) {
    char norm[FS_NAME_CAP];
    fs_dir_norm(dir, norm, sizeof(norm));
    fs_strcpy(cwd, norm, sizeof(cwd));
    sel = 0;
    scroll = 0;
    viewing = 0;
    push_history(cwd);
    refresh();
}

static void go_hist(int delta) {
    int np = hpos + delta;
    if (np < 0 || np >= hcount) return;
    hpos = np;
    fs_strcpy(cwd, hist[hpos], sizeof(cwd));
    sel = 0;
    scroll = 0;
    viewing = 0;
    refresh();
}

static void open_viewer(const char *path) {
    long n = fs_read_text(path, vbuf, (long)sizeof(vbuf));
    vlen = n < 0 ? 0 : n;
    fs_strcpy(vpath, path, sizeof(vpath));
    viewing = 1;
    vscroll = 0;
}

static void activate(int idx) {
    if (idx < 0 || idx >= nent) return;
    char path[256];
    fs_join(cwd, ents[idx].name, ents[idx].is_dir, path, sizeof(path));
    if (ents[idx].is_dir) go_dir(path);
    else if (kinds[idx] != 2) open_viewer(path);
    else set_msg("binary file - no viewer");
}

static void move_sel(int d) {
    if (nent == 0) return;
    sel += d;
    if (sel < 0) sel = 0;
    if (sel >= nent) sel = nent - 1;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + rows_visible()) scroll = sel - rows_visible() + 1;
    if (scroll < 0) scroll = 0;
}

// ---------- polished File Explorer UI ---------------------------------------
static int ends_ci(const char *n, const char *suf) {
    size_t ln = fs_strlen(n), ls = fs_strlen(suf);
    if (ln < ls) return 0;
    const char *p = n + ln - ls;
    for (size_t i = 0; i < ls; i++) {
        char a = p[i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static const char *icon_for(int is_dir, const char *name) {
    if (is_dir) {
        if (fs_streq(name, "Documents")) return "documents";
        if (fs_streq(name, "Downloads")) return "download-icon";
        return "folder-icon";
    }
    if (ends_ci(name, ".txt") || ends_ci(name, ".md") || ends_ci(name, ".log")) return "txt-file";
    if (ends_ci(name, ".png") || ends_ci(name, ".jpg") || ends_ci(name, ".bmp")) return "png-file";
    if (ends_ci(name, ".py")) return "python-file";
    if (ends_ci(name, ".cpp") || ends_ci(name, ".c") || ends_ci(name, ".hpp") || ends_ci(name, ".h"))
        return "cpp-file-icon";
    if (ends_ci(name, ".asm") || ends_ci(name, ".s")) return "asm-file-icon";
    return "basic-file";
}

static void draw_nav_button(GuiSurface *s, int i, int enabled, int dir) {
    int x, y, w, h;
    nav_rect(i, &x, &y, &w, &h);
    unsigned state = enabled ? 0u : GUI_STATE_DISABLED;
    gfx_button(s, x, y, w, h, "", g_theme, 0, state);
    unsigned ink = enabled ? g_theme->text : g_theme->faint;
    if (i == 3) {
        if (!gfx_icon_fit(s, "update-icon", x + (w - 16) / 2, y + (h - 16) / 2, 16))
            gfx_icon_refresh(s, x + w / 2, y + h / 2, ink);
    } else {
        gfx_chevron(s, x + w / 2, y + h / 2, dir, ink);
    }
}

static void draw_breadcrumb(GuiSurface *s) {
    int x, y, w, h;
    addr_rect(&x, &y, &w, &h);
    gfx_input(s, x, y, w, h, "", "", g_theme, GUI_STATE_DISABLED);
    int tx = x + 12, ty = TXTY(y, h), limit = x + w - 8;
    /* At the filesystem root, Home alone is the current location. */
    gfx_icon_home(s, x + 8, y + (h - 16) / 2, g_theme->accent);
    tx = x + 31;
    if (!cwd[0]) {
        gfx_text_clip(s, tx, ty, limit, "Home", g_theme->text);
        return;
    }
    gfx_text(s, tx, ty, "Home", g_theme->dim);
    tx += gfx_text_w("Home") + 7;
    gfx_chevron(s, tx, y + h / 2, 1, g_theme->faint);
    tx += 12;
    char segs[8][32];
    int nseg = 0, si = 0;
    for (int i = 0;; i++) {
        if (cwd[i] && cwd[i] != '/') {
            if (si < 31) segs[nseg][si++] = cwd[i];
            continue;
        }
        if (si && nseg < 8) {
            segs[nseg][si] = 0;
            nseg++;
            si = 0;
        }
        if (!cwd[i] || nseg >= 8) break;
    }
    for (int k = 0; k < nseg; k++) {
        int last = k == nseg - 1;
        int segw = gfx_text_w(segs[k]);
        if (tx + segw > limit) {
            gfx_text_clip(s, tx, ty, limit, segs[k], last ? g_theme->text : g_theme->dim);
            break;
        }
        gfx_text(s, tx, ty, segs[k], last ? g_theme->text : g_theme->dim);
        tx += segw + 6;
        if (!last) {
            gfx_chevron(s, tx, y + h / 2, 1, g_theme->faint);
            tx += 12;
        }
    }
}

static void draw_theme_toggle(GuiSurface *s) {
    if (W < 360) return;
    int bx = W - 40, by = 5, cx = bx + 16, cy = by + 14;
    gfx_button(s, bx, by, 32, 28, "", g_theme, 0, 0);
    if (g_theme == &kExplorerDark) {
        gfx_dot(s, cx, cy, 3, g_theme->accent);
        gfx_line(s, cx, cy - 7, cx, cy - 5, g_theme->accent);
        gfx_line(s, cx, cy + 5, cx, cy + 7, g_theme->accent);
        gfx_line(s, cx - 7, cy, cx - 5, cy, g_theme->accent);
        gfx_line(s, cx + 5, cy, cx + 7, cy, g_theme->accent);
        gfx_line(s, cx - 5, cy - 5, cx - 4, cy - 4, g_theme->accent);
        gfx_line(s, cx + 4, cy - 4, cx + 5, cy - 5, g_theme->accent);
        gfx_line(s, cx - 5, cy + 5, cx - 4, cy + 4, g_theme->accent);
        gfx_line(s, cx + 4, cy + 4, cx + 5, cy + 5, g_theme->accent);
    } else {
        gfx_dot(s, cx, cy, 6, g_theme->accent);
        gfx_dot(s, cx + 3, cy - 2, 5, g_theme->surface);
    }
}

static void draw_toolbar(GuiSurface *s) {
    gfx_fill(s, 0, 0, W, TOOLBAR_H, g_theme->surface);
    gfx_hline(s, 0, TOOLBAR_H - 1, W, g_theme->rule);
    draw_nav_button(s, 0, hpos > 0, 0);
    draw_nav_button(s, 1, hpos + 1 < hcount, 1);
    draw_nav_button(s, 2, cwd[0] != 0, 2);
    draw_nav_button(s, 3, 1, 3);
    draw_breadcrumb(s);
    draw_theme_toggle(s);
}

static void draw_command_bar(GuiSurface *s) {
    gfx_fill(s, 0, TOOLBAR_H, W, COMMAND_H, g_theme->surface);
    gfx_hline(s, 0, TOOLBAR_H + COMMAND_H - 1, W, g_theme->rule);
    for (int i = 0; i < 4; i++) {
        int bx, by, bw, bh;
        action_rect(i, &bx, &by, &bw, &bh);
        unsigned state = ((i == 2 || i == 3) && nent == 0) ? GUI_STATE_DISABLED : 0;
        gfx_button(s, bx, by, bw, bh, action_label(i), g_theme, i == 0, state);
    }
}

static void draw_sidebar(GuiSurface *s) {
    int sw = side_w();
    gfx_fill(s, 0, CONTENT_TOP, sw, H - CONTENT_TOP - STATUS_H, g_theme->surface);
    gfx_fill(s, sw - 1, CONTENT_TOP, 1, H - CONTENT_TOP - STATUS_H, g_theme->rule);
    for (int i = 0; i < kQuickN; i++) {
        int x, y, w, h;
        quick_rect(i, &x, &y, &w, &h);
        int active;
        if (i == 0) active = cwd[0] == 0;
        else {
            const char *prefix = kQuickPath[i];
            int j = 0;
            while (prefix[j] && cwd[j] == prefix[j]) j++;
            active = prefix[j] == 0;
        }
        if (active) {
            gfx_round(s, x, y, w, h, R_CTRL, g_theme->sel, 255);
            gfx_round(s, x, y + 7, 3, h - 14, 1, g_theme->accent, 255);
        } else {
            /* A subtle hover-like surface gives rows rhythm without extra labels. */
            gfx_hline(s, x + 8, y + h - 1, w - 16, g_theme->rule);
        }
        unsigned ink = active ? g_theme->text : g_theme->dim;
        int ix = x + 9, iy = y + (h - 20) / 2;
        if (i == 0) {
            gfx_icon_home(s, ix + 1, iy + 2, active ? g_theme->accent : g_theme->dim);
        } else if (i == 1) {
            if (!gfx_icon_fit(s, "folder-icon", ix, iy, 20)) gfx_icon_folder(s, ix + 2, iy + 2, g_theme->folder);
        } else if (i == 2) {
            if (!gfx_icon_fit(s, "computer", ix, iy, 20)) gfx_icon_drive(s, ix + 2, iy + 2, g_theme->dim);
        } else {
            if (!gfx_icon_fit(s, "memory_icon", ix, iy, 20)) gfx_icon_file(s, ix + 2, iy + 2, g_theme->binary);
        }
        const char *label = (W < 500 && i == 3) ? "Proc." : kQuickName[i];
        gfx_text_clip(s, x + 36, TXTY(y, h), x + w - 8, label, ink);
    }
}

static void draw_list_header(GuiSurface *s) {
    int x = list_card_x(), w = list_card_w(), y = list_card_y();
    gfx_fill(s, x, y, w, HDR_H, g_theme->card);
    gfx_text(s, x + 18, TXTY(y, HDR_H), "Name", g_theme->dim);
    if (W >= 520) {
        int divider = x + w - 104;
        gfx_fill(s, divider, y + 4, 1, HDR_H - 8, g_theme->rule);
        gfx_text(s, divider + 12, TXTY(y, HDR_H), "Size", g_theme->dim);
    }
    gfx_hline(s, x, y + HDR_H - 1, w, g_theme->rule);
}

static void draw_row(GuiSurface *s, int idx, int y) {
    int x = list_card_x() + 8;
    int w = list_card_w() - 16;
    int selected = idx == sel;
    int kind = kinds[idx];
    if (selected) {
        gfx_round(s, x, y, w, ROW_H - 2, R_ROW, g_theme->sel, 255);
        gfx_round(s, x + 1, y + 6, 2, ROW_H - 14, 1, g_theme->accent, 255);
        gfx_round(s, x, y, w, ROW_H - 2, R_ROW, g_theme->border, 115);
    }
    int ix = x + 10, iy = y + (ROW_H - 20) / 2;
    unsigned icon_color = ents[idx].is_dir ? g_theme->folder : (kind == 2 ? g_theme->binary : g_theme->textfile);
    if (!gfx_icon_fit(s, icon_for(ents[idx].is_dir, ents[idx].name), ix, iy, 20)) {
        if (ents[idx].is_dir) gfx_icon_folder(s, ix + 2, iy + 2, icon_color);
        else gfx_icon_file(s, ix + 2, iy + 2, icon_color);
    }
    int ty = TXTY(y, ROW_H - 2);
    int name_limit = x + w - (W >= 520 ? 106 : 8);
    gfx_text_clip(s, ix + 28, ty, name_limit, ents[idx].name, selected ? g_theme->text : g_theme->dim);
    if (W >= 520) {
        char sz[20];
        if (!ents[idx].is_dir) gfx_fmt_size(sz, ents[idx].size);
        else sz[0] = 0;
        gfx_text_clip(s, x + w - 92, ty, x + w - 8, sz, selected ? g_theme->text : g_theme->dim);
    }
    if (!selected && idx + 1 < nent) gfx_hline(s, x + 10, y + ROW_H - 2, w - 20, g_theme->rule);
}

static void draw_empty_state(GuiSurface *s) {
    int cx = list_card_x() + list_card_w() / 2;
    int cy = list_card_y() + list_card_h() / 2 - 12;
    gfx_dot(s, cx, cy - 5, 22, g_theme->surface);
    if (!gfx_icon_fit(s, "folder-icon", cx - 12, cy - 17, 24)) gfx_icon_folder(s, cx - 8, cy - 9, g_theme->folder);
    const char *a = "This folder is empty";
    gfx_text(s, cx - gfx_text_w(a) / 2, cy + 24, a, g_theme->dim);
}

static void draw_list(GuiSurface *s) {
    gfx_fill(s, list_card_x(), list_card_y(), list_card_w(), list_card_h(), g_theme->card);
    draw_list_header(s);
    if (nent <= 0) {
        draw_empty_state(s);
        return;
    }
    int vis = rows_visible();
    for (int r = 0; r < vis; r++) {
        int idx = scroll + r;
        if (idx >= nent) break;
        draw_row(s, idx, list_y0() + r * ROW_H);
    }
    gfx_scrollbar(s, list_card_x() + list_card_w() - 5, list_y0(), list_y1() - list_y0(), nent,
                  vis, scroll, g_theme->rule, g_theme->accent);
}

// One pass counts wrapped visual lines; draw=1 paints the visible viewport.
static int viewer_pass(GuiSurface *s, int draw, int top, int lines, int maxw, int tx, int ty) {
    int cur = 0, x = 0, i = 0;
    while (i < vlen) {
        char c = vbuf[i];
        if (c == '\n') { cur++; i++; x = 0; continue; }
        int j = i;
        while (j < vlen && vbuf[j] != ' ' && vbuf[j] != '\n') j++;
        int wl = j - i;
        if (wl <= 0) { i++; continue; }
        if (x > 0 && x + gfx_text_n_w(vbuf + i, wl) > maxw) { cur++; x = 0; }
        for (int k = 0; k < wl; k++) {
            const UiGlyph *g = gfx_glyph((unsigned char)(vbuf[i + k] == '\t' ? ' ' : vbuf[i + k]));
            if (x > 0 && x + g->adv > maxw) { cur++; x = 0; }
            if (draw && cur >= top && cur - top < lines)
                gfx_glyph_at(s, tx + x, ty + (cur - top) * VIEW_LINE, g, g_theme->text);
            x += g->adv;
        }
        i = j;
        if (i < vlen && vbuf[i] == ' ') {
            int sa = gfx_glyph(' ')->adv;
            if (x + sa <= maxw) x += sa;
            i++;
        }
    }
    return cur + 1;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    return base;
}

static void draw_viewer(GuiSurface *s) {
    int x = list_card_x(), y = list_card_y(), w = list_card_w(), h = list_card_h();
    gfx_fill(s, x, y, w, h, g_theme->card);
    gfx_icon_file(s, x + 14, y + 10, g_theme->accent);
    gfx_text_clip(s, x + 38, TXTY(y + 4, 28), x + w - 86, path_basename(vpath), g_theme->text);
    int bx = x + w - 66;
    gfx_button(s, bx, y + 4, 54, 28, "Close", g_theme, 0, 0);
    gfx_hline(s, x, y + 36, w, g_theme->rule);
    int tx = x + 14, ty = y + 45;
    int lines = (h - 55) / VIEW_LINE;
    if (lines < 1) lines = 1;
    int maxw = w - 34;
    if (maxw < 24) maxw = 24;
    int total = viewer_pass(s, 0, 0, 0, maxw, 0, 0);
    int maxs = total - lines;
    if (maxs < 0) maxs = 0;
    if (vscroll > maxs) vscroll = maxs;
    if (vscroll < 0) vscroll = 0;
    viewer_pass(s, 1, vscroll, lines, maxw, tx, ty);
    gfx_scrollbar(s, x + w - 6, y + 40, h - 44, total, lines, vscroll, g_theme->rule, g_theme->accent);
}

static void draw_status(GuiSurface *s) {
    int y = H - STATUS_H;
    gfx_fill(s, 0, y, W, STATUS_H, g_theme->surface);
    gfx_hline(s, 0, y, W, g_theme->rule);
    char num[12];
    gfx_itoa(num, (unsigned long)nent);
    int ty = TXTY(y, STATUS_H);
    gfx_text(s, 12, ty, num, g_theme->dim);
    gfx_text(s, 12 + gfx_text_w(num) + 5, ty, nent == 1 ? "item" : "items", g_theme->faint);
}

static void draw_toast(GuiSurface *s) {
    if (!g_msg[0]) return;
    if (choco_ticks() - g_msg_when > 220) { g_msg[0] = 0; return; }
    int maxw = W - 24;
    int w = gfx_text_w(g_msg) + 28;
    if (w > maxw) w = maxw;
    int h = 30, x = W - w - 12, y = H - STATUS_H - h - 10;
    gfx_elevate(s, x, y, w, h, R_CTRL, 16);
    gfx_panel(s, x, y, w, h, R_CTRL, g_theme->sel, g_theme->accent, 255);
    gfx_text_clip(s, x + 12, TXTY(y, h), x + w - 8, g_msg, g_theme->text);
}

static void draw_prompt(GuiSurface *s) {
    const char *title = g_prompt == 1 ? "Create a new file"
                       : g_prompt == 2 ? "Create a new folder"
                       : g_prompt == 3 ? "Rename item"
                                       : "Delete item";
    int bx, by;
    prompt_rect(&bx, &by);
    int pw = prompt_width(), ph = prompt_height();
    gfx_blend_alpha(s, 0, 0, W, H, g_theme->bg_bot, 170);
    gfx_elevate(s, bx, by, pw, ph, R_CARD, 30);
    gfx_card(s, bx, by, pw, ph, g_theme, 0);
    gfx_text(s, bx + 18, by + 14, title, g_theme->text);
    if (g_prompt != 4) {
        gfx_input(s, bx + 18, by + 48, pw - 36, 30, g_input, "Name", g_theme,
                  GUI_STATE_FOCUSED);
    } else {
        gfx_text_clip(s, bx + 18, by + 42, bx + pw - 16, "This can't be undone.", g_theme->danger);
    }
    int buttons_y = by + ph - 38;
    int cancel_w = pw >= 300 ? 78 : 62;
    int primary_w = g_prompt == 4 ? (pw >= 300 ? 78 : 62) : 62;
    int primary_x = bx + pw - 16 - primary_w;
    int cancel_x = primary_x - 7 - cancel_w;
    gfx_button(s, cancel_x, buttons_y, cancel_w, 26, "Cancel", g_theme, 0, 0);
    gfx_button(s, primary_x, buttons_y, primary_w, 26,
               g_prompt == 4 ? "Delete" : (g_prompt == 3 ? "Save" : "Create"), g_theme, 1, 0);
}

static void render(void) {
    GuiSurface *s = &g_s;
    gfx_fill(s, 0, 0, W, H, g_theme->bg_top);
    draw_toolbar(s);
    draw_command_bar(s);
    draw_sidebar(s);
    if (viewing) {
        // The preview owns the list card area, while navigation remains available.
        draw_viewer(s);
    } else {
        draw_list(s);
    }
    draw_status(s);
    draw_toast(s);
    if (g_prompt) draw_prompt(s);
}

// ---------- input ----------
static void do_up(void) {
    char par[FS_NAME_CAP];
    fs_parent(cwd, par, sizeof(par));
    go_dir(par);
}

static void start_prompt(int kind) {
    g_prompt = kind;
    g_ilen = 0;
    g_input[0] = 0;
    if (kind == 4) { g_input[0] = 'n'; g_input[1] = 0; g_ilen = 1; }
}

static void apply_prompt(void) {
    char path[256], npath[256];
    if (g_prompt == 1 || g_prompt == 2) {
        if (g_ilen) {
            fs_join(cwd, g_input, 0, path, sizeof(path));
            if (g_prompt == 1) {
                long fd = choco_open_flags(path, 0x40 | 0x01);  // O_CREAT | O_WRONLY
                if (fd >= 0) { choco_write(fd, "", 0); choco_close(fd); set_msg("file created"); }
                else set_msg("cannot create here");
            } else {
                set_msg(choco_mkdir(path) == 0 ? "folder created" : "cannot mkdir here");
            }
        }
    } else if (g_prompt == 3 && sel < nent && g_ilen) {
        fs_join(cwd, ents[sel].name, ents[sel].is_dir, path, sizeof(path));
        fs_join(cwd, g_input, ents[sel].is_dir, npath, sizeof(npath));
        set_msg(choco_rename(path, npath) == 0 ? "renamed" : "cannot rename here");
    } else if (g_prompt == 4 && sel < nent && (g_input[0] == 'y' || g_input[0] == 'Y')) {
        fs_join(cwd, ents[sel].name, ents[sel].is_dir, path, sizeof(path));
        long r = ents[sel].is_dir ? choco_rmdir(path) : choco_unlink(path);
        set_msg(r == 0 ? "Deleted" : "Cannot delete item");
    }
    g_prompt = 0;
    g_ilen = 0;
    g_input[0] = 0;
    refresh();
}

static void handle_key(char c) {
    if (g_prompt) {
        if (c == 0x1b) { g_prompt = 0; g_ilen = 0; g_input[0] = 0; return; }
        if (c == '\n' || c == '\r') { apply_prompt(); return; }
        if (c == 0x08 || c == 0x7f) { if (g_ilen) g_input[--g_ilen] = 0; return; }
        if (g_prompt == 4) {
            if (c == 'y' || c == 'Y' || c == 'n' || c == 'N') {
                g_input[0] = c; g_input[1] = 0; g_ilen = 1;
            }
            return;
        }
        if (c >= 32 && c < 127 && g_ilen < 60) { g_input[g_ilen++] = c; g_input[g_ilen] = 0; }
        return;
    }
    if (viewing) {
        if (c == 0x1b || c == 'q') viewing = 0;
        else if (c == 'j' || c == 's') vscroll++;
        else if (c == 'k' || c == 'w') vscroll--;
        return;
    }
    switch (c) {
        case 'q': case 0x1b: g_quit = 1; break;
        case 'h': case 'H': go_dir(""); break;
        case 'u': case 0x08: do_up(); break;
        case '[': go_hist(-1); break;
        case ']': go_hist(1); break;
        case 'r': case 'R': refresh(); set_msg("Refreshed"); break;
        case 'l': case 'L': g_theme = toggle_explorer_theme(g_theme); set_msg("Theme changed"); break;
        case 'f': {
            compact = !compact;
            long r = choco_win_resize(g_addr, compact ? 480 : 620, compact ? 340 : 460);
            if (r != 0) { compact = !compact; set_msg("Resize refused"); }
            else set_msg(compact ? "Compact view" : "Full view");
            break;
        }
        case 'j': case 'J': move_sel(1); break;
        case 'k': case 'K': move_sel(-1); break;
        case 'n': start_prompt(1); break;
        case 'N': start_prompt(2); break;
        case 'm': case 'M': if (nent) start_prompt(3); break;
        case 'd': case 'D': if (nent) start_prompt(4); break;
        case '\n': case '\r': activate(sel); break;
        default: break;
    }
}

static void handle_click(int x, int y) {
    if (g_prompt) {
        int bx, by;
        prompt_rect(&bx, &by);
        int pw = prompt_width(), ph = prompt_height();
        int buttons_y = by + ph - 38;
        int cancel_w = pw >= 300 ? 78 : 62;
        int primary_w = g_prompt == 4 ? (pw >= 300 ? 78 : 62) : 62;
        int primary_x = bx + pw - 16 - primary_w;
        int cancel_x = primary_x - 7 - cancel_w;
        if (y >= buttons_y && y < buttons_y + 26) {
            if (x >= cancel_x && x < cancel_x + cancel_w) {
                g_prompt = 0; g_ilen = 0; g_input[0] = 0; return;
            }
            if (x >= primary_x && x < primary_x + primary_w) {
                // Clicking Delete is an explicit destructive confirmation.
                if (g_prompt == 4) { g_input[0] = 'y'; g_input[1] = 0; g_ilen = 1; }
                apply_prompt();
                return;
            }
        }
        return;
    }
    if (W >= 360 && y >= 5 && y < 33 && x >= W - 40 && x < W - 8) {
        g_theme = toggle_explorer_theme(g_theme);
        set_msg("Theme changed");
        return;
    }
    if (viewing) {
        int vx = list_card_x(), vy = list_card_y(), vw = list_card_w();
        if (x >= vx + vw - 66 && x < vx + vw - 12 && y >= vy + 4 && y < vy + 32) viewing = 0;
        return;
    }
    if (y >= 0 && y < TOOLBAR_H) {
        for (int i = 0; i < 4; i++) {
            int bx, by, bw, bh;
            nav_rect(i, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                if (i == 0) go_hist(-1);
                else if (i == 1) go_hist(1);
                else if (i == 2) do_up();
                else { refresh(); set_msg("Refreshed"); }
                return;
            }
        }
        return;
    }
    if (y >= TOOLBAR_H && y < CONTENT_TOP) {
        for (int i = 0; i < 4; i++) {
            int bx, by, bw, bh;
            action_rect(i, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                if (i == 0) start_prompt(1);
                else if (i == 1) start_prompt(2);
                else if (i == 2 && nent) start_prompt(3);
                else if (i == 3 && nent) start_prompt(4);
                return;
            }
        }
        return;
    }
    if (x < side_w() && y >= CONTENT_TOP) {
        for (int i = 0; i < kQuickN; i++) {
            int bx, by, bw, bh;
            quick_rect(i, &bx, &by, &bw, &bh);
            if (y >= by && y < by + bh) { go_dir(kQuickPath[i]); return; }
        }
        return;
    }
    if (y >= list_y0() && y < list_y1() && x >= list_card_x()) {
        int row = scroll + (y - list_y0()) / ROW_H;
        if (row < 0 || row >= nent) return;
        long now = choco_ticks();
        if (row == g_last_row && now - g_last_tick < 40) {
            activate(row);
            g_last_row = -1;
        } else {
            sel = row;
            g_last_row = row;
            g_last_tick = now;
        }
    }
}

// The WM owns resizing; it tells us the new content size through UEV_RESIZE.
// Only the visible extent changes - the buffer keeps its capacity stride.
static void apply_size(int w, int h) {
    if (w < 200) w = 200;
    if (h < 140) h = 140;
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    W = w;
    H = h;
    g_s.w = W;
    g_s.h = H;
    refresh();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    long addr = choco_win_create(W, H, "File Explorer");
    if (addr <= 0) {
        choco_print("fileman: win_create failed\n");
        return 1;
    }
    g_win = (struct uw_win *)addr;
    g_addr = addr;
    g_s.px = (unsigned *)(addr + UW_PIX_OFF);
    g_s.w = W;
    g_s.h = H;
    g_s.pitch = (int)g_win->pitch;
    if (g_s.pitch < W) g_s.pitch = W;  // pre-capacity server: stride == width
    cwd[0] = 0;
    refresh();
    push_history(cwd);

    while (1) {
        while (g_win->tail != g_win->head) {
            struct uw_ev e = g_win->ev[g_win->tail % 32];
            g_win->tail++;
            if (e.type == UEV_KEY) handle_key((char)e.info);
            else if (e.type == UEV_MOUSE) handle_click((int)e.x, (int)e.y);
            else if (e.type == UEV_RESIZE) apply_size((int)e.x, (int)e.y);
            if (g_quit) { choco_win_close(addr); return 0; }
        }
        render();
        choco_win_present(addr);
        choco_sleep(40);
    }
}

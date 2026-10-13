// user/lib/gui.h - Fluent-style drawing primitives for ring-3 apps.
// Header-only, freestanding, integer only (no libc, no FPU: the build uses
// -mno-80387 -mno-sse). The pixel buffer starts at (win_addr + UW_PIX_OFF)
// and is w*h uint32 pixels, stride = w.
//
// Look: "Midnight Amethyst" - Fluent (Win11) structure painted with the
// ChocoOS palette from kernel/include/choco/theme.hpp. The window manager
// rounds the outer window corners, so apps paint rectangular content.
#pragma once
#include "font_ui.h"
#include "icons_ui.h"

// ---- design tokens -------------------------------------------------------
#define UI_BG_TOP     0x0F0E1Eu // modern dark gradient top
#define UI_BG_BOT     0x0A0813u // modern dark gradient bottom
#define UI_CARD       0x13121Eu // elevated content surface
#define UI_SURFACE    0x1A1927u // chrome / nav surface (elevated)
#define UI_HOVER      0x23202Fu // pointer-over state
#define UI_SEL        0x2D2A40u // selected row / active nav
#define UI_BORDER     0x2F2C3Du // refined control border
#define UI_RULE       0x26232Fu // refined hairline separators
#define UI_TEXT       0xF5F3FBu // primary text (enhanced brightness)
#define UI_TEXT_DIM   0xA39AB5u // secondary text
#define UI_TEXT_FAINT 0x7A728Bu // tertiary / captions
#define UI_ACCENT     0x7B68EEu // primary accent (medium slate blue)
#define UI_ACCENT_HI  0xC8B6FFu // accent light (focus stripe)
#define UI_ACCENT_DK  0x5A4AB5u // accent dark (pressed)
#define UI_FOLDER     0xFFA726u // warm accent (folder icon)
#define UI_TEXTFILE   0x81C784u // success accent (file icon)
#define UI_BINARY     0x64B5F6u // info accent (binary icon)
#define UI_DANGER     0xEF5350u // alert accent (error state)

#define UI_GLASS_BG   0x1A1927u // glassmorphism background
#define UI_GLASS_BG_DIM 0x0F0E1Eu // glassmorphism dim

#define UI_SER_CPU    0x9575CDu // amethyst: CPU (the headline metric)
#define UI_SER_MEM    0x4DB8E8u // bright cyan: memory
#define UI_SER_DISK   0x81C784u // fresh green: disk
#define UI_SER_NET_RX 0xFFB74Du // warm amber: network in
#define UI_SER_NET_TX 0xF06292u // vibrant pink: network out
#define UI_SER_PROC   0xCE93D8u // light purple: per-process
#define UI_SER_WARN   0xEF5350u // vivid red: threshold crossed
#define UI_SER_GRID   0x26232Fu // refined grid hairline

#define R_ROW  5
#define R_CTRL 7
#define R_CARD 10

typedef struct GuiSurface {
    unsigned *px;
    int w, h;      // visible size
    int pitch;     // row stride; 0 means "same as w"
} GuiSurface;

static inline int gfx_stride(const GuiSurface *s) { return s->pitch ? s->pitch : s->w; }

// ---- blending ------------------------------------------------------------
// Linear blend of `c` over `d` with alpha al (0..255), per 8-bit channel.
static inline unsigned gfx_mix(unsigned d, unsigned c, unsigned al) {
    if (al >= 255) return c;
    unsigned ia = 255 - al;
    unsigned rb = ((((d & 0xFF0000u) * ia + (c & 0xFF0000u) * al) >> 8) & 0xFF0000u);
    unsigned gr = ((((d & 0x00FF00u) * ia + (c & 0x00FF00u) * al) >> 8) & 0x00FF00u);
    unsigned bl = (((d & 0x0000FFu) * ia + (c & 0x0000FFu) * al) >> 8) & 0x0000FFu;
    return rb | gr | bl;
}

static inline void gfx_put(GuiSurface *s, int x, int y, unsigned c) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px[y * gfx_stride(s) + x] = c;
}

static inline void gfx_blend(GuiSurface *s, int x, int y, unsigned c, unsigned al) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    unsigned d = s->px[y * gfx_stride(s) + x];
    s->px[y * gfx_stride(s) + x] = gfx_mix(d, c, al);
}

static inline void gfx_blend_alpha(GuiSurface *s, int x, int y, int w, int h, unsigned c,
                                   unsigned al) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->w ? s->w : x + w;
    int y1 = y + h > s->h ? s->h : y + h;
    if (al >= 255) {
        for (int j = y0; j < y1; j++)
            for (int i = x0; i < x1; i++) s->px[j * gfx_stride(s) + i] = c;
        return;
    }
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) gfx_blend(s, i, j, c, al);
}

static inline void gfx_fill(GuiSurface *s, int x, int y, int w, int h, unsigned c) {
    gfx_blend_alpha(s, x, y, w, h, c, 255);
}

// Vertical gradient: the "Mica" base layer.
static inline void gfx_vgrad(GuiSurface *s, int x, int y, int w, int h, unsigned top,
                             unsigned bot) {
    if (h <= 1) { gfx_fill(s, x, y, w, h, top); return; }
    for (int j = 0; j < h; j++) {
        unsigned a = (unsigned)j * 255u / (unsigned)(h - 1);
        gfx_fill(s, x, y + j, w, 1, gfx_mix(top, bot, a));
    }
}

// Rounded rectangle, corners anti-aliased with 4x4 integer supersampling.
static inline void gfx_round(GuiSurface *s, int x, int y, int w, int h, int r, unsigned c,
                             unsigned al) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 1) { gfx_blend_alpha(s, x, y, w, h, c, al); return; }
    const int r8 = r * 8;                 // sub-pixel units (1/8 px)
    const int rr = r8 * r8;
    for (int j = 0; j < h; j++) {
        int zone = -1;
        if (j < r) zone = j;
        else if (j >= h - r) zone = h - 1 - j;
        if (zone < 0) {
            gfx_blend_alpha(s, x, y + j, w, 1, c, al);
            continue;
        }
        gfx_blend_alpha(s, x + r, y + j, w - 2 * r, 1, c, al);
        for (int i = 0; i < r; i++) {
            int cov = 0;
            for (int sy = 0; sy < 4; sy++) {
                int qy = zone * 8 + 2 * sy + 1;
                int dy = r8 - qy;
                for (int sx = 0; sx < 4; sx++) {
                    int qx = i * 8 + 2 * sx + 1;
                    int dx = r8 - qx;
                    if (dx * dx + dy * dy <= rr) cov++;
                }
            }
            if (!cov) continue;
            unsigned aa = (unsigned)cov * 255u / 16u * al / 255u;
            gfx_blend(s, x + i, y + j, c, aa);
            gfx_blend(s, x + w - 1 - i, y + j, c, aa);
        }
    }
}

// Filled rounded panel with a 1px border (Fluent control look).
static inline void gfx_panel(GuiSurface *s, int x, int y, int w, int h, int r,
                             unsigned fill, unsigned border, unsigned al) {
    gfx_round(s, x, y, w, h, r, border, al);
    gfx_round(s, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : r, fill, al);
}

static inline void gfx_hline(GuiSurface *s, int x, int y, int w, unsigned c) {
    gfx_fill(s, x, y, w, 1, c);
}

// ---- text (proportional, anti-aliased: baked Inter) ----------------------
static inline const UiGlyph *gfx_glyph(unsigned char ch) {
    if (ch < UIFONT_FIRST || ch > UIFONT_LAST) ch = '?';
    return &uifont_glyphs[ch - UIFONT_FIRST];
}

// y is the TOP of the line box (UIFONT_LINE tall).
static inline void gfx_glyph_at(GuiSurface *s, int x, int y, const UiGlyph *g, unsigned fg) {
    if (!g->w || !g->h) return;
    const uint8_t *a = uifont_alpha + g->off;
    int gx = x + g->bx, gy = y + g->by;
    for (int j = 0; j < g->h; j++) {
        for (int i = 0; i < g->w; i++) {
            unsigned al = a[j * g->w + i];
            if (al) gfx_blend(s, gx + i, gy + j, fg, al);
        }
    }
}

static inline void gfx_char(GuiSurface *s, int x, int y, unsigned char ch, unsigned fg) {
    gfx_glyph_at(s, x, y, gfx_glyph(ch), fg);
}

static inline void gfx_text(GuiSurface *s, int x, int y, const char *t, unsigned fg) {
    for (; *t; t++) {
        const UiGlyph *g = gfx_glyph((unsigned char)*t);
        gfx_glyph_at(s, x, y, g, fg);
        x += g->adv;
    }
}

// Width of the first n bytes of t.
static inline int gfx_text_n_w(const char *t, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += gfx_glyph((unsigned char)t[i])->adv;
    return w;
}

static inline int gfx_text_w(const char *t) {
    int w = 0;
    for (; *t; t++) w += gfx_glyph((unsigned char)*t)->adv;
    return w;
}

static inline void gfx_text_bg(GuiSurface *s, int x, int y, const char *t, unsigned fg,
                               unsigned bg) {
    gfx_fill(s, x, y, gfx_text_w(t), UIFONT_LINE, bg);
    gfx_text(s, x, y, t, fg);
}

// Clipped text: stops before x_limit (keeps a label inside its column).
static inline void gfx_text_clip(GuiSurface *s, int x, int y, int x_limit, const char *t,
                                 unsigned fg) {
    for (; *t; t++) {
        const UiGlyph *g = gfx_glyph((unsigned char)*t);
        if (x + g->adv > x_limit) break;
        gfx_glyph_at(s, x, y, g, fg);
        x += g->adv;
    }
}

static inline void gfx_text_right(GuiSurface *s, int x_right, int y, const char *t,
                                  unsigned fg) {
    gfx_text(s, x_right - gfx_text_w(t), y, t, fg);
}

// Vertically center one line of text inside a box.
#define TXTY(box_y, box_h) ((box_y) + ((box_h) - UIFONT_LINE) / 2)

// dir: 0 left, 1 right, 2 up, 3 down
static inline void gfx_chevron(GuiSurface *s, int cx, int cy, int dir, unsigned c) {
    for (int i = 0; i < 5; i++) {
        switch (dir) {
            case 0: gfx_put(s, cx + 2 - i, cy - 4 + i, c); gfx_put(s, cx + 2 - i, cy + 4 - i, c); break;
            case 1: gfx_put(s, cx - 2 + i, cy - 4 + i, c); gfx_put(s, cx - 2 + i, cy + 4 - i, c); break;
            case 2: gfx_put(s, cx - 4 + i, cy + 2 - i, c); gfx_put(s, cx + 4 - i, cy + 2 - i, c); break;
            default: gfx_put(s, cx - 4 + i, cy - 2 + i, c); gfx_put(s, cx + 4 - i, cy - 2 + i, c); break;
        }
    }
}

// ---- icons (16x16 cell) --------------------------------------------------
static inline void gfx_icon_folder(GuiSurface *s, int x, int y, unsigned c) {
    gfx_round(s, x + 1, y + 3, 7, 4, 1, c, 255);
    gfx_round(s, x + 1, y + 5, 14, 10, 2, c, 255);
    gfx_round(s, x + 2, y + 8, 12, 5, 1, 0x000000u, 36);
}

static inline void gfx_icon_file(GuiSurface *s, int x, int y, unsigned c) {
    gfx_round(s, x + 3, y + 1, 10, 14, 2, c, 255);
    gfx_fill(s, x + 10, y + 1, 3, 3, UI_BG_BOT);
    gfx_put(s, x + 10, y + 4, UI_BORDER);
    gfx_put(s, x + 11, y + 3, UI_BORDER);
    gfx_put(s, x + 12, y + 2, UI_BORDER);
}

static inline void gfx_icon_drive(GuiSurface *s, int x, int y, unsigned c) {
    gfx_round(s, x + 1, y + 4, 14, 9, 2, c, 255);
    gfx_fill(s, x + 3, y + 8, 6, 1, UI_BG_BOT);
    gfx_put(s, x + 12, y + 9, UI_BG_TOP);
}

static inline void gfx_icon_home(GuiSurface *s, int x, int y, unsigned c) {
    for (int i = 0; i < 7; i++) {
        gfx_put(s, x + 8 - i, y + 2 + i, c);
        gfx_put(s, x + 8 + i, y + 2 + i, c);
    }
    gfx_round(s, x + 4, y + 8, 9, 7, 1, c, 255);
    gfx_fill(s, x + 7, y + 11, 3, 4, UI_BG_BOT);
}

// Circular arrow: unmistakably "refresh", never mistaken for a forward chevron.
static inline void gfx_icon_refresh(GuiSurface *s, int cx, int cy, unsigned c) {
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > 25 || d2 < 9) continue;
            if (dx >= 2 && dy <= -2) continue;  // opening for the arrow head
            gfx_put(s, cx + dx, cy + dy, c);
        }
    gfx_fill(s, cx + 2, cy - 7, 4, 2, c);
    gfx_put(s, cx + 5, cy - 4, c);
    gfx_put(s, cx + 4, cy - 3, c);
}

// ---- bitmap icons baked from icons/ (24x24 RGBA, see build-assets.py) -----
static inline const uint8_t *gfx_icon_find(const char *name) {
    for (int i = 0; i < kUiIconCount; i++) {
        const char *a = kUiIcons[i].name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) return kUiIcons[i].rgba;
    }
    return 0;
}

// Alpha blit of a baked icon at native size, with an extra opacity multiplier
// (255 = full). Returns 0 when the asset is missing so callers can fall back
// to the drawn shapes above.
static inline int gfx_icon_mul(GuiSurface *s, const char *name, int x, int y, int mul) {
    const uint8_t *rgba = gfx_icon_find(name);
    if (!rgba) return 0;
    for (int j = 0; j < UIICON_SIZE; j++) {
        for (int i = 0; i < UIICON_SIZE; i++) {
            const int p = (j * UIICON_SIZE + i) * 4;
            unsigned a = (unsigned)rgba[p + 3] * (unsigned)mul / 255u;
            if (!a) continue;
            const unsigned c = ((unsigned)rgba[p] << 16) | ((unsigned)rgba[p + 1] << 8) |
                               (unsigned)rgba[p + 2];
            gfx_blend(s, x + i, y + j, c, a);
        }
    }
    return 1;
}

static inline int gfx_icon_at(GuiSurface *s, const char *name, int x, int y) {
    return gfx_icon_mul(s, name, x, y, 255);
}

// Area-averaged draw into a dst x dst square. Nearest-neighbour would break
// the thin line-art when shrinking, so each output pixel averages its source
// footprint (colour is weighted by alpha).
static inline int gfx_icon_fit(GuiSurface *s, const char *name, int x, int y, int dst) {
    const uint8_t *rgba = gfx_icon_find(name);
    if (!rgba || dst <= 0) return 0;
    if (dst == UIICON_SIZE) return gfx_icon_at(s, name, x, y);
    for (int j = 0; j < dst; j++) {
        int sy0 = j * UIICON_SIZE / dst, sy1 = (j + 1) * UIICON_SIZE / dst;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int i = 0; i < dst; i++) {
            int sx0 = i * UIICON_SIZE / dst, sx1 = (i + 1) * UIICON_SIZE / dst;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const int p = (sy * UIICON_SIZE + sx) * 4;
                    const unsigned a = rgba[p + 3];
                    sr += (unsigned)rgba[p] * a;
                    sg += (unsigned)rgba[p + 1] * a;
                    sb += (unsigned)rgba[p + 2] * a;
                    sa += a;
                    cnt++;
                }
            if (!sa) continue;
            const unsigned c = ((sr / sa) << 16) | ((sg / sa) << 8) | (sb / sa);
            gfx_blend(s, x + i, y + j, c, sa / cnt);
        }
    }
    return 1;
}

// ---- numbers -------------------------------------------------------------
static inline int gfx_itoa(char *buf, unsigned long v) {
    char rev[24];
    int r = 0, p = 0;
    if (v == 0) buf[p++] = '0';
    else {
        while (v) { rev[r++] = (char)('0' + v % 10); v /= 10; }
        while (r) buf[p++] = rev[--r];
    }
    buf[p] = 0;
    return p;
}
// Human-readable byte size into buf (e.g. "2.1 MB"). buf >= 16 bytes.
static inline void gfx_fmt_size(char *buf, unsigned long n) {
    int p = 0;
    unsigned long whole = n, frac = 0;
    const char *unit = "B";
    if (n >= 1024UL * 1024UL) {
        whole = n / (1024UL * 1024UL);
        frac = (n / (1024UL * 102UL)) % 10;
        unit = "MB";
    } else if (n >= 1024UL) {
        whole = n / 1024UL;
        frac = (n / 102UL) % 10;
        unit = "KB";
    }
    p += gfx_itoa(buf + p, whole);
    if (unit[1]) { buf[p++] = '.'; buf[p++] = (char)('0' + frac); }
    buf[p++] = ' ';
    buf[p++] = unit[0];
    if (unit[1]) buf[p++] = unit[1];
    buf[p] = 0;
}

// ---- charts --------------------------------------------------------------
// Everything here is integer math: there is no FPU in ring 3 (-mno-80387
// -mno-sse), so no trig. That rules out donuts/gauges and is exactly why real
// system monitors (Task Manager, GNOME System Monitor) use area charts, bars
// and meters - all of which map cleanly onto columns and rects.
//
// Values are always 0..vmax, mapped into the plot box; series longer than the
// box are averaged down, never clipped, so a 120-sample history fits a 90px
// chart without losing the peaks.

static inline void gfx_line(GuiSurface *s, int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = ax - ay;
    int x = x0, y = y0;
    // The bound is not decoration: with a wrong err seed the endpoint can be
    // stepped over, and an endless loop here hangs the whole machine (there is
    // no watchdog, and the caller is a painting app that never returns).
    for (int guard = ax + ay + 2; guard > 0; guard--) {
        gfx_put(s, x, y, c);
        if (x == x1 && y == y1) return;
        int e2 = 2 * err;
        if (e2 > -ay) { err -= ay; x += sx; }
        if (e2 < ax) { err += ax; y += sy; }
    }
    gfx_put(s, x1, y1, c);
}

static inline void gfx_elevate(GuiSurface *s, int x, int y, int w, int h, int r, int strength) {
    for (int i = 4; i >= 1; i--) {
        unsigned al = (unsigned)(strength * (5 - i) / 5);
        gfx_round(s, x - i, y - i, w + 2 * i, h + 2 * i, r + i, 0x000000u, al);
    }
}

static inline void gfx_glass_panel(GuiSurface *s, int x, int y, int w, int h, int r,
                                   unsigned fill, unsigned border, unsigned al) {
    gfx_round(s, x, y, w, h, r, border, al);
    gfx_round(s, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : r, fill, (unsigned)(al * 220 / 255));
    if (h >= 4) gfx_fill(s, x + 1, y + 1, w - 2, 1, gfx_mix(fill, 0xFFFFFFu, 12));
}

// Rounded track + fill, the workhorse "how full is it" meter. pct is 0..100.
static inline void gfx_meter(GuiSurface *s, int x, int y, int w, int h, int pct,
                             unsigned track, unsigned fill) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_round(s, x, y, w, h, h / 2, track, 255);
    if (!pct) return;
    int fw = (int)((long)w * pct / 100);
    if (fw < h) fw = h;               // never lose a non-zero reading
    if (fw > w) fw = w;
    gfx_round(s, x, y, fw, h, h / 2, fill, 255);
    // 1px light cap on the leading edge reads as gloss, not as a flat block.
    if (fw > 2) gfx_fill(s, x + fw - 2, y + 1, 1, h - 2, gfx_mix(fill, 0xFFFFFFu, 70));
}

// Horizontal stacked meter: parts[] are percentages of the same whole.
static inline void gfx_meter_stack(GuiSurface *s, int x, int y, int w, int h,
                                   const int *parts, const unsigned *colors, int n) {
    gfx_round(s, x, y, w, h, h / 2, UI_RULE, 255);
    int acc = 0;
    for (int i = 0; i < n; i++) {
        int x0 = (int)((long)w * acc / 100);
        acc += parts[i];
        if (acc > 100) acc = 100;
        int x1 = (int)((long)w * acc / 100);
        if (x1 > x0) gfx_blend_alpha(s, x + x0, y, x1 - x0, h, colors[i], 255);
        if (acc >= 100) break;
    }
}

// Graph paper: the grid sells the scale before the numbers do.
static inline void gfx_grid(GuiSurface *s, int x, int y, int w, int h, int vlines,
                            int hlines, unsigned c) {
    for (int i = 1; i < hlines; i++)
        gfx_hline(s, x, y + (int)((long)h * i / hlines), w, c);
    for (int i = 1; i < vlines; i++)
        gfx_fill(s, x + (int)((long)w * i / vlines), y, 1, h, c);
}

// Downsample n values into `out` columns by averaging (keeps peaks honest).
static inline void gfx_resample(const unsigned *v, int n, unsigned *out, int cols,
                                int vmax) {
    for (int c = 0; c < cols; c++) {
        int i0 = (int)((long)n * c / cols);
        int i1 = (int)((long)n * (c + 1) / cols);
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > n) i1 = n;
        unsigned long acc = 0;
        int cnt = 0;
        for (int i = i0; i < i1; i++) { acc += v[i]; cnt++; }
        unsigned val = cnt ? (unsigned)(acc / (unsigned long)cnt) : 0;
        if (val > (unsigned)vmax) val = (unsigned)vmax;
        out[c] = val;
    }
}

// Filled area chart with a brighter stroke: CPU/net history.
// `v` holds n samples of 0..vmax, oldest first. Returns nothing.
static inline void gfx_area_chart(GuiSurface *s, int x, int y, int w, int h,
                                  const unsigned *v, int n, int vmax, unsigned stroke,
                                  unsigned fill, unsigned al, unsigned *scratch,
                                  int scratch_n) {
    if (w <= 1 || h <= 1 || n <= 0 || vmax <= 0) return;
    int cols = w;
    const unsigned *cv = v;
    if (cols > scratch_n) cols = scratch_n;
    if (n != cols) {
        gfx_resample(v, n, scratch, cols, vmax);
        cv = scratch;
    }
    int prev_y = -1;
    for (int c = 0; c < cols; c++) {
        int hh = (int)((long)h * (long)cv[c] / vmax);
        if (hh > h) hh = h;
        int top = y + h - hh;
        if (hh > 0) gfx_blend_alpha(s, x + c, top, 1, hh, fill, al);
        if (prev_y >= 0 && prev_y != top) gfx_line(s, x + c - 1, prev_y, x + c, top, stroke);
        else if (prev_y >= 0) gfx_put(s, x + c, top, stroke);
        prev_y = top;
    }
}

// Column chart (per-core usage, histograms): n bars spread over w.
static inline void gfx_bars(GuiSurface *s, int x, int y, int w, int h, const unsigned *v,
                            int n, int vmax, unsigned bar, unsigned track) {
    if (n <= 0 || w <= 0 || h <= 0) return;
    int slot = w / n;
    if (slot < 1) slot = 1;
    int bw = slot - (n > 8 ? 1 : 2);
    if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++) {
        int bx = x + i * slot;
        if (bx + bw > x + w) break;
        if (track) gfx_fill(s, bx, y, bw, h, track);
        unsigned val = v[i];
        if (vmax > 0 && val > (unsigned)vmax) val = (unsigned)vmax;
        int hh = vmax > 0 ? (int)((long)h * (long)val / vmax) : 0;
        if (hh > h) hh = h;
        if (hh > 0) gfx_fill(s, bx, y + h - hh, bw, hh, bar);
    }
}

// Status LED.
static inline void gfx_dot(GuiSurface *s, int cx, int cy, int r, unsigned c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            int d2 = i * i + j * j;
            if (d2 <= r * r) gfx_put(s, cx + i, cy + j, c);
            else if (d2 <= r * r + r) gfx_blend(s, cx + i, cy + j, c, 90);
        }
}

// Thin Fluent scrollbar: thumb position is derived from the visible window.
static inline void gfx_scrollbar(GuiSurface *s, int x, int y, int h, int total, int visible,
                                 int top, unsigned track, unsigned thumb) {
    if (h <= 4 || total <= visible) return;
    gfx_round(s, x, y, 4, h, 2, track, 90);
    int th = (int)((long)h * visible / total);
    if (th < 18) th = 18;
    if (th > h) th = h;
    int ty = y + (int)((long)(h - th) * top / (total - visible > 0 ? total - visible : 1));
    if (ty + th > y + h) ty = y + h - th;
    if (ty < y) ty = y;
    gfx_round(s, x, ty, 4, th, 2, thumb, 220);
}


// ============================================================================
// ChocoOS UI Toolkit - Phase 2
// Flex layout, reusable controls, theme objects, and integer-only animations.
// All routines are header-only and ring-3/freestanding friendly. Controls are
// render primitives: hit-testing, focus ownership, and text editing remain the
// responsibility of the calling app/event loop.
// ============================================================================

typedef struct GuiTheme {
    unsigned bg_top, bg_bot, card, surface, hover, sel, border, rule;
    unsigned text, dim, faint, accent, accent_hi, accent_dk;
    unsigned folder, textfile, binary, glass_bg, glass_bg_dim;
    unsigned cpu, mem, disk, rx, tx, proc, warn, danger, grid, wr;
} GuiTheme;

// Central shared themes. Keep role ordering identical with GuiTheme above.
static const GuiTheme GUI_THEME_MIDNIGHT_DARK = {
    0x0F0E1E, 0x0A0813, 0x13121E, 0x1A1927, 0x23202F, 0x2D2A40, 0x2F2C3D, 0x26232F,
    0xF5F3FB, 0xA39AB5, 0x7A728B, 0x7B68EE, 0xC8B6FF, 0x5A4AB5,
    0xFFA726, 0x81C784, 0x64B5F6, 0x1A1927, 0x0F0E1E,
    0x9575CD, 0x4DB8E8, 0x81C784, 0xFFB74D, 0xF06292, 0xCE93D8, 0xEF5350, 0xEF5350, 0x26232F, 0x7CFC00
};

static const GuiTheme GUI_THEME_MIDNIGHT_LIGHT = {
    0xF8F7FEu, 0xF0EFFE, 0xFDFCFF, 0xF4F2FB, 0xECEAF8, 0xE4E0F5, 0xD9D5EE, 0xE8E4F2,
    0x1A1820, 0x4E4560, 0x66607B, 0x5B4FBF, 0x4A3FA8, 0x45388F,
    0xB86B00, 0x2B7C3C, 0x2367A0, 0xF4F2FB, 0xF8F7FE,
    0x6B4FB5, 0x0B5F8F, 0x2B7C3C, 0xD97706, 0xB63F7C, 0x8C4DAA, 0xC62828, 0xDC2626, 0xD9D5EE, 0x4D7C0F
};

static inline const GuiTheme *gui_theme_dark(void) { return &GUI_THEME_MIDNIGHT_DARK; }
static inline const GuiTheme *gui_theme_light(void) { return &GUI_THEME_MIDNIGHT_LIGHT; }
static inline const GuiTheme *gui_theme_toggle(const GuiTheme *current) {
    return current == &GUI_THEME_MIDNIGHT_LIGHT ? &GUI_THEME_MIDNIGHT_DARK : &GUI_THEME_MIDNIGHT_LIGHT;
}

typedef struct GuiRect { int x, y, w, h; } GuiRect;

enum {
    GUI_FLEX_ROW = 0,
    GUI_FLEX_COLUMN = 1
};
enum {
    GUI_JUSTIFY_START = 0,
    GUI_JUSTIFY_CENTER,
    GUI_JUSTIFY_END,
    GUI_JUSTIFY_SPACE_BETWEEN,
    GUI_JUSTIFY_SPACE_AROUND
};
enum {
    GUI_ALIGN_START = 0,
    GUI_ALIGN_CENTER,
    GUI_ALIGN_END,
    GUI_ALIGN_STRETCH
};

typedef struct GuiFlexStyle {
    int direction;          // GUI_FLEX_ROW or GUI_FLEX_COLUMN
    int justify;             // main-axis placement
    int align;               // cross-axis placement
    int gap;                 // base gap between children, pixels
    int pad_left, pad_top, pad_right, pad_bottom;
} GuiFlexStyle;

typedef struct GuiFlexItem {
    int basis;               // preferred main-axis size
    int cross;               // preferred cross-axis size; ignored by STRETCH
    int grow;                // positive free-space weight
    int shrink;              // negative free-space weight (0 disables shrink)
    int min_size;            // main-axis minimum; <= 0 means 0
    int max_size;            // main-axis maximum; <= 0 means unbounded
    GuiRect rect;            // output rectangle
} GuiFlexItem;

static inline int gui_iabs(int v) { return v < 0 ? -v : v; }
static inline int gui_imin(int a, int b) { return a < b ? a : b; }
static inline int gui_imax(int a, int b) { return a > b ? a : b; }
static inline int gui_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (hi >= 0 && v > hi) return hi;
    return v;
}

// Lay out n items inside a rectangle. `basis`, `cross`, and constraints are in
// pixels. The layout is deterministic integer arithmetic, with no allocation.
// Returns the number of laid-out items (0 for invalid input).
static inline int gui_flex_layout(GuiRect box, const GuiFlexStyle *style,
                                  GuiFlexItem *items, int n) {
    if (!style || !items || n <= 0 || box.w <= 0 || box.h <= 0) return 0;
    int row = style->direction != GUI_FLEX_COLUMN;
    int pad_a = row ? style->pad_left : style->pad_top;
    int pad_b = row ? style->pad_right : style->pad_bottom;
    int cross_a = row ? style->pad_top : style->pad_left;
    int cross_b = row ? style->pad_bottom : style->pad_right;
    int outer_main = row ? box.w : box.h;
    int outer_cross = row ? box.h : box.w;
    int inner_main = gui_imax(0, outer_main - gui_imax(0,pad_a) - gui_imax(0,pad_b));
    int inner_cross = gui_imax(0, outer_cross - gui_imax(0,cross_a) - gui_imax(0,cross_b));
    int gap = gui_imax(0, style->gap);
    if (n > 1 && gap > inner_main / (n - 1)) gap = inner_main / (n - 1);
    int base_gaps = gap * (n - 1);
    int available = gui_imax(0, inner_main - base_gaps);
    int used = 0, grow_sum = 0, shrink_sum = 0;
    int i;
    for (i = 0; i < n; i++) {
        int minv = gui_imax(0, items[i].min_size);
        int maxv = items[i].max_size > 0 ? items[i].max_size : -1;
        int b = gui_clamp(gui_imax(0, items[i].basis), minv, maxv);
        items[i].basis = b;
        used += b;
        if (items[i].grow > 0) grow_sum += items[i].grow;
        if (items[i].shrink > 0) shrink_sum += items[i].shrink * (b > 0 ? b : 1);
    }
    if (used < available && grow_sum > 0) {
        int extra = available - used, distributed = 0;
        for (i = 0; i < n; i++) {
            if (items[i].grow <= 0) continue;
            int add = (int)((long)extra * items[i].grow / grow_sum);
            int maxv = items[i].max_size > 0 ? items[i].max_size : -1;
            int next = gui_clamp(items[i].basis + add, gui_imax(0,items[i].min_size), maxv);
            distributed += next - items[i].basis;
            items[i].basis = next;
        }
        // Allocate integer-division remainder deterministically from the start.
        for (i = 0; i < n && distributed < extra; i++) {
            int maxv = items[i].max_size > 0 ? items[i].max_size : -1;
            if (items[i].grow > 0 && (maxv < 0 || items[i].basis < maxv)) {
                items[i].basis++;
                distributed++;
            }
        }
    } else if (used > available && shrink_sum > 0) {
        int deficit = used - available, removed = 0;
        for (i = 0; i < n; i++) {
            if (items[i].shrink <= 0 || items[i].basis <= 0) continue;
            int weight = items[i].shrink * (items[i].basis > 0 ? items[i].basis : 1);
            int take = (int)((long)deficit * weight / shrink_sum);
            int floorv = gui_imax(0, items[i].min_size);
            if (take > items[i].basis - floorv) take = items[i].basis - floorv;
            items[i].basis -= take;
            removed += take;
        }
        for (i = n - 1; i >= 0 && removed < deficit; i--) {
            int floorv = gui_imax(0, items[i].min_size);
            if (items[i].shrink > 0 && items[i].basis > floorv) {
                items[i].basis--;
                removed++;
            }
        }
    }
    used = base_gaps;
    for (i = 0; i < n; i++) used += items[i].basis;
    int free_main = inner_main - used;
    int leading = 0, extra_gap = 0;
    if (free_main > 0) {
        if (style->justify == GUI_JUSTIFY_CENTER) leading = free_main / 2;
        else if (style->justify == GUI_JUSTIFY_END) leading = free_main;
        else if (style->justify == GUI_JUSTIFY_SPACE_BETWEEN && n > 1) extra_gap = free_main / (n - 1);
        else if (style->justify == GUI_JUSTIFY_SPACE_AROUND && n > 0) {
            extra_gap = free_main / n;
            leading = extra_gap / 2;
        }
    }
    int cursor = (row ? box.x : box.y) + gui_imax(0,pad_a) + leading;
    int cross_origin = (row ? box.y : box.x) + gui_imax(0,cross_a);
    for (i = 0; i < n; i++) {
        int cs = style->align == GUI_ALIGN_STRETCH ? inner_cross : items[i].cross;
        if (cs <= 0) cs = inner_cross;
        if (cs > inner_cross) cs = inner_cross;
        int co = cross_origin;
        if (style->align == GUI_ALIGN_CENTER) co += (inner_cross - cs) / 2;
        else if (style->align == GUI_ALIGN_END) co += inner_cross - cs;
        if (row) {
            items[i].rect.x = cursor; items[i].rect.y = co;
            items[i].rect.w = items[i].basis; items[i].rect.h = cs;
        } else {
            items[i].rect.x = co; items[i].rect.y = cursor;
            items[i].rect.w = cs; items[i].rect.h = items[i].basis;
        }
        cursor += items[i].basis + gap + extra_gap;
    }
    return n;
}

enum {
    GUI_STATE_HOVERED  = 1u << 0,
    GUI_STATE_PRESSED  = 1u << 1,
    GUI_STATE_DISABLED = 1u << 2,
    GUI_STATE_FOCUSED  = 1u << 3,
    GUI_STATE_SELECTED = 1u << 4
};

// Reusable paint-only controls. Pass GUI_STATE_* flags derived from app input.
static inline void gfx_card(GuiSurface *s, int x, int y, int w, int h,
                            const GuiTheme *t, int elevated) {
    if (!s || !t || w <= 0 || h <= 0) return;
    if (elevated) gfx_elevate(s, x, y, w, h, R_CARD, 18);
    gfx_glass_panel(s, x, y, w, h, R_CARD, t->card, t->border, 244);
}

static inline void gfx_button(GuiSurface *s, int x, int y, int w, int h,
                              const char *label, const GuiTheme *t,
                              int primary, unsigned state) {
    if (!s || !t || !label || w <= 0 || h <= 0) return;
    unsigned fill = primary ? t->accent : t->surface;
    unsigned edge = primary ? t->accent : t->border;
    unsigned ink = primary ? t->text : t->text;
    if (state & GUI_STATE_DISABLED) {
        fill = t->card; edge = t->rule; ink = t->faint;
    } else if (state & GUI_STATE_PRESSED) {
        fill = primary ? t->accent_dk : t->sel;
        edge = primary ? t->accent_dk : t->accent;
    } else if (state & GUI_STATE_HOVERED) {
        fill = primary ? t->accent_hi : t->hover;
        edge = t->accent_hi;
        ink = primary ? t->bg_bot : t->text;
    }
    gfx_panel(s, x, y, w, h, R_CTRL, fill, edge, 255);
    if (state & GUI_STATE_FOCUSED) {
        gfx_round(s, x + 2, y + 2, w - 4, h - 4, R_CTRL > 2 ? R_CTRL - 2 : 1,
                  t->accent_hi, 210);
    }
    int tx = x + (w - gfx_text_w(label)) / 2;
    int ty = y + (h - UIFONT_LINE) / 2;
    if (tx < x + 6) tx = x + 6;
    gfx_text_clip(s, tx, ty, x + w - 6, label, ink);
}

static inline void gfx_input(GuiSurface *s, int x, int y, int w, int h,
                             const char *value, const char *placeholder,
                             const GuiTheme *t, unsigned state) {
    if (!s || !t || w <= 0 || h <= 0) return;
    unsigned fill = (state & GUI_STATE_DISABLED) ? t->card : t->surface;
    unsigned edge = (state & GUI_STATE_FOCUSED) ? t->accent : t->border;
    unsigned ink = (state & GUI_STATE_DISABLED) ? t->faint : t->text;
    gfx_panel(s, x, y, w, h, R_CTRL, fill, edge, 255);
    const char *shown = (value && value[0]) ? value : placeholder;
    if (!shown) shown = "";
    unsigned shown_ink = (value && value[0]) ? ink : t->faint;
    int tx = x + 10, ty = y + (h - UIFONT_LINE) / 2;
    gfx_text_clip(s, tx, ty, x + w - 9, shown, shown_ink);
    if ((state & GUI_STATE_FOCUSED) && !(state & GUI_STATE_DISABLED) && value && value[0]) {
        int cx = tx + gfx_text_w(value) + 1;
        if (cx < x + w - 8) gfx_fill(s, cx, y + 7, 1, h - 14, t->accent_hi);
    }
}

enum {
    GUI_EASE_LINEAR = 0,
    GUI_EASE_OUT_QUAD,
    GUI_EASE_IN_OUT_QUAD
};

typedef struct GuiAnimation {
    int from, to, value;
    unsigned long start_ms, duration_ms;
    unsigned char easing, active;
} GuiAnimation;

static inline void gui_anim_begin(GuiAnimation *a, int from, int to,
                                  unsigned long now_ms, unsigned long duration_ms,
                                  unsigned char easing) {
    if (!a) return;
    a->from = from; a->to = to; a->value = from;
    a->start_ms = now_ms; a->duration_ms = duration_ms ? duration_ms : 1;
    a->easing = easing; a->active = (from != to) ? 1 : 0;
}

// Advance an animation and return its current integer value. Call from the
// app's frame/tick loop; no timer or global state is hidden in the toolkit.
static inline int gui_anim_tick(GuiAnimation *a, unsigned long now_ms) {
    if (!a) return 0;
    if (!a->active) return a->value;
    unsigned long elapsed = now_ms - a->start_ms; // unsigned wrap-safe clock delta
    if (elapsed >= a->duration_ms) {
        a->value = a->to; a->active = 0; return a->value;
    }
    long q = (long)(elapsed * 1000UL / a->duration_ms);
    if (a->easing == GUI_EASE_OUT_QUAD) {
        long inv = 1000 - q; q = 1000 - inv * inv / 1000;
    } else if (a->easing == GUI_EASE_IN_OUT_QUAD) {
        if (q < 500) q = q * q * 2 / 1000;
        else { long inv = 1000 - q; q = 1000 - inv * inv * 2 / 1000; }
    }
    a->value = a->from + (int)((long)(a->to - a->from) * q / 1000);
    return a->value;
}

static inline int gui_anim_is_active(const GuiAnimation *a) {
    return a ? (int)a->active : 0;
}

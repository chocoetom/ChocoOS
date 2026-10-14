#include <stddef.h>

extern "C" void *memcpy(void *d, const void *s, size_t n);
#include <choco/boot.hpp>
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/font.hpp>
#include <choco/gui.hpp>
#include <choco/icon.hpp>
#include <choco/mem.hpp>
#include <choco/mouse.hpp>
#include <choco/sched.hpp>
#include <choco/shortcuts.hpp>
#include <choco/theme.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

namespace choco::gui {
long user_close(uint64_t uaddr);
namespace {
using namespace theme;

namespace pal {
constexpr uint32_t TITLE_ACTIVE = 0x2A2640;
constexpr uint32_t TITLE_IDLE = 0x1E1B2D;
constexpr uint32_t EDGE_ACTIVE = 0x5A4BA6;
constexpr uint32_t EDGE_IDLE = 0x302C46;
constexpr uint32_t TASKBAR = 0x14111E;
constexpr uint32_t TASK_EDGE = 0x2D2942;
constexpr uint32_t PANEL = 0x1C1929;
constexpr uint32_t PANEL_FOOT = 0x151220;
constexpr uint32_t PANEL_EDGE = 0x3B3657;
constexpr uint32_t FIELD = 0x2A2640;
constexpr uint32_t FIELD_EDGE = 0x3F3A62;
constexpr uint32_t TILE = 0x2E2A48;
constexpr uint32_t TILE_HI = 0x3A3560;
constexpr uint32_t TEXT = 0xF3F1FB;
constexpr uint32_t TEXT_DIM = 0x9F9BBA;
constexpr uint32_t ACCENT = 0x8B5CF6;
constexpr uint32_t ACCENT_HI = 0xB197FC;
constexpr uint32_t ACCENT_LO = 0x6D3FD6;
constexpr uint32_t DANGER = 0xC42B1C;
constexpr uint32_t WHITE = 0xFFFFFF;
constexpr uint32_t BLACK = 0x000000;
} // namespace pal

// Back buffer: desenha em RAM, copia para a tela no final de cada frame.
// Elimina tearing (quadro parcial visível) e flicker.
constexpr bool kUseBackBuffer = true;
uint32_t *s_back = nullptr;    // Back buffer (RAM)
uint32_t *s_fb = nullptr;      // Framebuffer real (VRAM)
uint32_t s_fb_pitch = 0;       // Pitch real do framebuffer

// Only the terminal is a kernel-owned window now: the monitor became a user
// program (user/sysmon.c) and the Files window was replaced by bin/fileman.
enum WinKind : uint8_t { WK_TERM = 0, WK_N = 1, WK_USER = 1 };
constexpr int kWinR = 8;
constexpr int kMenuR = 12;
constexpr int kCapW = 46;
constexpr int kCornerMax = 12;
constexpr int kCovMax = 18;

const int kTitleH = (int)TITLE_H;
const int kBorder = (int)BORDER;
const int kTaskH = (int)TASK_H;
const int kCellW = (int)CELL_W;
const int kCellH = (int)CELL_H;
const int kShadowOff = (int)SHADOW_OFF;
const int kShadowR = (int)SHADOW_R;

struct Surface {
    uint32_t *d = nullptr;
    int w = 0, h = 0, pitch = 0;
};

struct Rect {
    int x, y, w, h;
};

struct Window {
    int x = 0, y = 0;
    uint64_t w = 0, h = 0;
    char title[40]{};
    char prog[24]{};      // program that owns it, for taskbar/shortcut matching
    uint32_t *px = nullptr;
    bool visible = false, minimized = false;
    bool is_term = false;
    WinKind kind = WK_TERM;
    int uw = -1;
    // Restore geometry for maximize / Aero snap.
    int rx = 0, ry = 0;
    uint64_t rw = 0, rh = 0;
    bool maximized = false;
};

Surface s_scr;
uint32_t *s_wall = nullptr;
Window s_win[WK_N]{};
Window s_uwin[4]{};
Window *s_focused = nullptr;
bool s_dirty = true;          // Full frame redraw flag
uint64_t s_last_seq = 0, s_last_sec = 0, s_last_paint = 0;
int s_lastmx = -1, s_lastmy = -1;
uint8_t s_lastb = 0;
bool s_cursor_moved = false;  // pointer moved this tick -> cursor-only repaint
// Save-under for the pointer: the box around it is copied before the glyph is
// drawn, so a cursor-only update can erase the old position without recomposing
// the whole scene. It must cover the full glyph extent (the arrow spans
// x..x+10 by y..y+16) plus the pad, or the uncovered edge leaves a pixel trail.
constexpr int kCurPad = 8, kCurBox = 28;
uint32_t s_curbg[kCurBox * kCurBox];
int s_curx = 0, s_cury = 0;
bool s_curon = false;
Window *s_drag = nullptr;
int s_grabdx = 0, s_grabdy = 0;

// Resize / maximize / Aero-snap state (phase 1: kernel windows only).
enum : unsigned { EZ_NONE = 0, EZ_L = 1, EZ_R = 2, EZ_T = 4, EZ_B = 8 };
constexpr int kGrab = 6;      // border band that starts a resize
constexpr int kMinW = 320;    // smallest outer window width
constexpr int kMinH = 200;    // smallest outer window height
constexpr int kResizeStep = 4;  // quantise the drag to bound buffer churn
Window *s_rz = nullptr;       // window being resized
unsigned s_rz_zone = EZ_NONE;
int s_rz_mx = 0, s_rz_my = 0;             // cursor at press
int s_rz_x = 0, s_rz_y = 0;               // geometry at press
uint64_t s_rz_w = 0, s_rz_h = 0;
unsigned s_cursor_zone = EZ_NONE;         // hover feedback for the pointer
int s_snap = 0;                           // 0 none, 1 left, 2 right, 3 top
Window *s_tclick = nullptr;               // double-click on the caption
uint64_t s_tclick_when = 0;

// Dirty rectangles: only redraw what changed (cursor, window move, etc.)
constexpr int kDirtyMax = 32;
struct DirtyRect { int x, y, w, h; bool used = false; };
DirtyRect g_dirty[kDirtyMax] = {};
int g_dirty_count = 0;

void mark_dirty(int x, int y, int w, int h) {
    if (g_dirty_count >= kDirtyMax) { s_dirty = true; return; }  // overflow -> full
    g_dirty[g_dirty_count].x = x;
    g_dirty[g_dirty_count].y = y;
    g_dirty[g_dirty_count].w = w;
    g_dirty[g_dirty_count].h = h;
    g_dirty[g_dirty_count].used = true;
    g_dirty_count++;
}

bool s_menu = false;
int s_sel_icon = -1;
int s_desk = -1;
uint64_t s_desk_when = 0;

uint8_t g_cov[kCovMax + 1][kCovMax][kCovMax];
bool g_cov_ready = false;

uint32_t mix(uint32_t dst, uint32_t src, unsigned a) {
    if (a >= 255) return src;
    if (!a) return dst;
    const unsigned ia = 255 - a;
    unsigned r = ((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia + 128;
    unsigned g = ((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia + 128;
    unsigned b = (src & 0xFF) * a + (dst & 0xFF) * ia + 128;
    r = (r + (r >> 8)) >> 8;
    g = (g + (g >> 8)) >> 8;
    b = (b + (b >> 8)) >> 8;
    return (r << 16) | (g << 8) | b;
}

unsigned isqrt32(unsigned v) {
    unsigned r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

void init_coverage() {
    if (g_cov_ready) return;
    for (int r = 1; r <= kCovMax; r++)
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                const int dx = 2 * (r - i) - 1, dy = 2 * (r - j) - 1;
                const unsigned d16 = isqrt32((unsigned)(dx * dx + dy * dy) * 256u);
                const int v = (2 * r + 1) * 16 - (int)d16;
                const int cov = v <= 0 ? 0 : (v >= 32 ? 255 : v * 255 / 32);
                g_cov[r][i][j] = (uint8_t)cov;
            }
    g_cov_ready = true;
}

int clamp_radius(int r, int w, int h) {
    const int limit = (w < h ? w : h) / 2;
    if (r > limit) r = limit;
    if (r > kCovMax) r = kCovMax;
    return r < 0 ? 0 : r;
}

bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

bool hovered(int x, int y, int w, int h) {
    return mouse::present() && in_rect(mouse::x(), mouse::y(), x, y, w, h);
}

void put(Surface &s, int x, int y, uint32_t c, unsigned a = 255) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
    uint32_t &p = s.d[(size_t)y * (size_t)s.pitch + (size_t)x];
    p = a >= 255 ? c : mix(p, c, a);
}

void fill_rect(Surface &s, int x, int y, int w, int h, uint32_t c, unsigned a = 255) {
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = x + w > s.w ? s.w : x + w, y1 = y + h > s.h ? s.h : y + h;
    if (x0 >= x1 || y0 >= y1 || !a) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &s.d[(size_t)yy * (size_t)s.pitch];
        if (a >= 255) {
            for (int xx = x0; xx < x1; xx++) row[xx] = c;
        } else {
            for (int xx = x0; xx < x1; xx++) row[xx] = mix(row[xx], c, a);
        }
    }
}

void round_rect(Surface &s, int x, int y, int w, int h, int r, uint32_t c, unsigned a = 255) {
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(r, w, h);
    if (r == 0) {
        fill_rect(s, x, y, w, h, c, a);
        return;
    }
    init_coverage();
    for (int j = 0; j < h; j++) {
        const int yy = y + j;
        if (yy < 0 || yy >= s.h) continue;
        const int zone = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        if (zone < 0) {
            fill_rect(s, x, yy, w, 1, c, a);
            continue;
        }
        fill_rect(s, x + r, yy, w - 2 * r, 1, c, a);
        for (int i = 0; i < r; i++) {
            const unsigned cov = g_cov[r][i][zone];
            if (!cov) continue;
            const unsigned aa = (cov * a + 127) / 255;
            put(s, x + i, yy, c, aa);
            put(s, x + w - 1 - i, yy, c, aa);
        }
    }
}

int text_w(const char *str, unsigned scale = 1) {
    return (int)font::text_width(str, scale);
}

// Top y that vertically centers one text line inside a box.
int vcenter(int box_y, int box_h, unsigned scale = 1) {
    return box_y + (box_h - (int)font::line_height() * (int)scale) / 2;
}

void text(Surface &s, const char *str, int x, int y, uint32_t c, unsigned scale = 1) {
    if (!str) return;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    // y is the TOP of the line box (font::line_height() tall). Glyphs carry
    // per-pixel coverage, so this path is anti-aliased; scale>2 is clamped to
    // a 2x nearest-neighbour upscale of the baked bitmap.
    uint8_t scratch[font::SCRATCH];
    font::Glyph g;
    for (; *str; str++) {
        font::get(*str, g, scratch);
        if (x >= s.w) break;
        const int gx = x + g.bx * (int)scale, gy = y + g.by * (int)scale;
        for (int j = 0; j < g.h; j++) {
            for (int i = 0; i < g.w; i++) {
                const unsigned v = g.bits[j * g.w + i];
                if (!v) continue;
                for (unsigned sy = 0; sy < scale; sy++)
                    for (unsigned sx = 0; sx < scale; sx++)
                        put(s, gx + i * (int)scale + (int)sx,
                            gy + j * (int)scale + (int)sy, c, v);
            }
        }
        x += g.adv * (int)scale;
    }
}

void text_right(Surface &s, const char *str, int right, int y, uint32_t c, unsigned scale = 1) {
    text(s, str, right - text_w(str, scale), y, c, scale);
}

void text_center(Surface &s, const char *str, int cx, int y, uint32_t c, unsigned scale = 1) {
    text(s, str, cx - text_w(str, scale) / 2, y, c, scale);
}

const icon::Asset *glyph(const char *name) {
    const icon::Asset *a = icon::find(name);
    return (a && a->w == 24 && a->h == 24) ? a : nullptr;
}

void draw_icon(Surface &s, const char *name, int x, int y) {
    const icon::Asset *a = glyph(name);
    if (!a) {
        round_rect(s, x + 4, y + 4, 16, 16, 4, pal::ACCENT);
        return;
    }
    icon::draw(s.d, (uint64_t)s.w, (uint64_t)s.h, (uint64_t)s.pitch, a, x, y);
}

uint64_t content_w(const Window &wd) {
    return wd.w > 2 * (uint64_t)kBorder ? wd.w - 2 * (uint64_t)kBorder : 0;
}
uint64_t content_h(const Window &wd) {
    return wd.h > (uint64_t)(kTitleH + kBorder) ? wd.h - (uint64_t)(kTitleH + kBorder) : 0;
}
bool shown(const Window &wd) { return wd.visible && !wd.minimized; }

void blit(const uint32_t *src, uint64_t sw, uint64_t sh, uint64_t sp, int dx, int dy) {
    if (!src) return;
    const uint64_t W = (uint64_t)s_scr.w, H = (uint64_t)s_scr.h, P = (uint64_t)s_scr.pitch;
    if (dx >= 0 && dy >= 0 && (uint64_t)dx + sw <= W && (uint64_t)dy + sh <= H) {
        for (uint64_t y = 0; y < sh; y++)
            memcpy(&s_scr.d[(uint64_t)(dy + (int)y) * P + (uint64_t)dx], &src[y * sp],
                   (size_t)(sw * 4));
        return;
    }
    for (uint64_t y = 0; y < sh; y++) {
        const int sy = dy + (int)y;
        if (sy < 0 || sy >= s_scr.h) continue;
        for (uint64_t x = 0; x < sw; x++) {
            const int sx = dx + (int)x;
            if (sx < 0 || sx >= s_scr.w) continue;
            s_scr.d[(uint64_t)sy * P + (uint64_t)sx] = src[y * sp + x];
        }
    }
}

struct CornerBuf {
    uint32_t px[4][kCornerMax * kCornerMax];
};

struct CornerGeo {
    int bx, by, sx, sy;
};

CornerGeo corner_geo(int c, int x, int y, int w, int h) {
    CornerGeo g;
    g.bx = (c == 0 || c == 2) ? x : x + w - 1;
    g.by = (c < 2) ? y : y + h - 1;
    g.sx = (c == 0 || c == 2) ? 1 : -1;
    g.sy = (c < 2) ? 1 : -1;
    return g;
}

void snap_corners(int x, int y, int w, int h, int r, CornerBuf &cb) {
    for (int c = 0; c < 4; c++) {
        const CornerGeo g = corner_geo(c, x, y, w, h);
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                const int qx = g.bx + g.sx * i, qy = g.by + g.sy * j;
                uint32_t v = 0;
                if (qx >= 0 && qy >= 0 && qx < s_scr.w && qy < s_scr.h)
                    v = s_scr.d[(size_t)qy * (size_t)s_scr.pitch + (size_t)qx];
                cb.px[c][j * r + i] = v;
            }
    }
}

void apply_corners(int x, int y, int w, int h, int r, const CornerBuf &cb, uint32_t edge) {
    init_coverage();
    for (int c = 0; c < 4; c++) {
        const CornerGeo g = corner_geo(c, x, y, w, h);
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                const unsigned cov = g_cov[r][i][j];
                const unsigned inner = (r >= 2 && i >= 1 && j >= 1) ? g_cov[r - 1][i - 1][j - 1] : 0;
                if (cov == 255 && inner == 255) continue;
                const int qx = g.bx + g.sx * i, qy = g.by + g.sy * j;
                if (qx < 0 || qy < 0 || qx >= s_scr.w || qy >= s_scr.h) continue;
                uint32_t &p = s_scr.d[(size_t)qy * (size_t)s_scr.pitch + (size_t)qx];
                uint32_t cur = p;
                if (inner < 255) cur = mix(cur, edge, 255 - inner);
                p = mix(cb.px[c][j * r + i], cur, cov);
            }
    }
}

void shade_px(int sx, int sy, unsigned strength) {
    if (sx < 0 || sy < 0 || sx >= s_scr.w || sy >= s_scr.h) return;
    if (strength > 256) strength = 256;
    uint32_t &p = s_scr.d[(size_t)sy * (size_t)s_scr.pitch + (size_t)sx];
    const unsigned inv = 256 - strength;
    p = ((((p >> 16) & 0xFF) * inv >> 8) << 16) | ((((p >> 8) & 0xFF) * inv >> 8) << 8) |
        (((p & 0xFF) * inv) >> 8);
}

void drop_shadow(int x, int y, uint64_t w, uint64_t h, unsigned level) {
    const int ox = x + kShadowOff, oy = y + kShadowOff;
    const int wi = (int)w, hi = (int)h;
    const unsigned umbra = 120 * level >> 8;
    for (int sy = oy; sy < oy + hi; sy++)
        for (int sx = ox + wi - kShadowOff; sx < ox + wi; sx++) shade_px(sx, sy, umbra);
    for (int sy = oy + hi - kShadowOff; sy < oy + hi; sy++)
        for (int sx = ox; sx < ox + wi - kShadowOff; sx++) shade_px(sx, sy, umbra);
    static unsigned kHalo[9]{};
    static bool kHaloInit = false;
    if (!kHaloInit) {
        for (int d = 0; d <= kShadowR && d < 9; d++)
            kHalo[d] = (unsigned)(64 * (kShadowR - d + 1) / (kShadowR + 1));
        kHaloInit = true;
    }
    for (int j = -kShadowR; j < 0; j++)
        for (int i = -kShadowR; i < wi + kShadowR; i++) {
            const int dist = -j + (i < 0 ? -i : (i >= wi ? i - wi + 1 : 0));
            if (dist > kShadowR) continue;
            shade_px(ox + i, oy + j, kHalo[dist] * level >> 8);
        }
    for (int j = hi; j < hi + kShadowR; j++)
        for (int i = -kShadowR; i < wi + kShadowR; i++) {
            const int dist = (j - hi + 1) + (i < 0 ? -i : (i >= wi ? i - wi + 1 : 0));
            if (dist > kShadowR) continue;
            shade_px(ox + i, oy + j, kHalo[dist] * level >> 8);
        }
    for (int j = 0; j < hi; j++)
        for (int i = -kShadowR; i < 0; i++) shade_px(ox + i, oy + j, kHalo[-i] * level >> 8);
    for (int j = 0; j < hi; j++)
        for (int i = wi; i < wi + kShadowR; i++)
            shade_px(ox + i, oy + j, kHalo[i - wi + 1] * level >> 8);
}

bool title_is(const char *t, const char *p) {
    while (*p) if (*t++ != *p++) return false;
    return true;
}

const char *win_icon(const Window &wd) {
    if (wd.kind == WK_TERM) return "terminal-icon";
    // User windows have no kind of their own: match the program by title so
    // /bin/fileman and /bin/sysmon get their real icons in the chrome.
    if (title_is(wd.title, "File Explorer")) return "file-explorer";
    if (title_is(wd.title, "System Monitor")) return "system-monitor";
    return "code_icon";
}

// Capacity is the whole screen's content area, so a window can be resized up
// to near-full-screen without ever remapping (must match UW_MAX_W/UW_MAX_H in
// user/lib.h). A maximized 1280x800 window has content 1276x770, so 1280x800
// leaves a little slack for the app asking for a size before the WM clamps it.
constexpr uint64_t UW_MAX_W = 1280, UW_MAX_H = 800, UW_PIX_OFF = 4096;
constexpr uint64_t UW_BASE = 0x800000ULL;
// 4 slots x ~4 MiB full-capacity regions = ~16 MiB; 24 MiB leaves headroom and
// stays well below the linux mmap arena at 0x70000000.
constexpr uint64_t UW_AREA = 24 * 1024 * 1024;
constexpr unsigned UW_MAX = 4, UW_EV_N = 32;
constexpr uint64_t UW_MAGIC = 0xC40C0A11ULL;
constexpr uint64_t UEV_MOUSE = 1, UEV_KEY = 2, UEV_RESIZE = 3;
// Every user window owns a full-capacity pixel region from the start, so a
// resize only changes the visible w/h: no frame is ever freed or remapped
// under a running app, and the app's pointer stays valid for its lifetime.
constexpr uint64_t UW_CAP_BYTES = UW_MAX_W * UW_MAX_H * 4;

struct UWEvent {
    uint64_t type = 0, x = 0, y = 0, info = 0;
};
struct UWHead {
    uint64_t magic = 0, w = 0, h = 0, dirty = 0, head = 0, tail = 0;
    UWEvent ev[UW_EV_N]{};
    uint64_t pitch = 0;  // row stride in pixels (= UW_MAX_W, the capacity)
};
struct UWSlot {
    bool used = false;
    uint64_t root = 0, uaddr = 0, w = 0, h = 0, nframes = 0, pitch = 0;
    uint32_t pid = 0;   // owning process, so the close button can kill it
    uint64_t *frames = nullptr;
    Window *win = nullptr;
};
UWSlot s_uw[UW_MAX]{};

UWHead *uw_head(UWSlot &u) {
    if (!u.frames) return nullptr;
    return (UWHead *)paging::phys_to_virt(u.frames[0]);
}

UWSlot *uw_find(uint64_t root, uint64_t uaddr) {
    for (unsigned i = 0; i < UW_MAX; i++)
        if (s_uw[i].used && s_uw[i].root == root && s_uw[i].uaddr == uaddr) return &s_uw[i];
    return nullptr;
}

void uw_push(UWSlot &u, uint64_t type, uint64_t x, uint64_t y, uint64_t info) {
    UWHead *hd = uw_head(u);
    if (!hd || hd->magic != UW_MAGIC) return;
    hd->ev[hd->head % UW_EV_N].type = type;
    hd->ev[hd->head % UW_EV_N].x = x;
    hd->ev[hd->head % UW_EV_N].y = y;
    hd->ev[hd->head % UW_EV_N].info = info;
    hd->head++;
}

void blit_user(UWSlot &u, int dx, int dy) {
    if (!u.frames || !u.w || !u.h) return;
    for (uint64_t y = 0; y < u.h; y++) {
        const int sy = dy + (int)y;
        if (sy < 0 || sy >= s_scr.h) continue;
        const int x0 = dx < 0 ? -dx : 0;
        int x1 = (int)u.w;
        const int maxx = s_scr.w - dx;
        if (maxx < x1) x1 = maxx;
        if (x0 >= x1) continue;
        // The buffer pitch is the capacity stride, not the visible width.
        uint64_t lin = y * (u.pitch ? u.pitch : u.w) + (uint64_t)x0;
        uint64_t npix = (uint64_t)(x1 - x0);
        uint8_t *dst = (uint8_t *)&s_scr.d[(uint64_t)sy * (uint64_t)s_scr.pitch + (uint64_t)(dx + x0)];
        if (x0 == 0 && npix == u.w) {
            uint64_t off = (lin % 1024) * 4;
            uint64_t page = 1 + lin / 1024;
            uint64_t left = npix * 4;
            while (left && page < u.nframes) {
                uint64_t chunk = 4096 - off;
                if (chunk > left) chunk = left;
                memcpy(dst, (uint8_t *)paging::phys_to_virt(u.frames[page]) + off, (size_t)chunk);
                dst += chunk;
                left -= chunk;
                off = 0;
                page++;
            }
            continue;
        }
        while (npix) {
            uint64_t page = 1 + lin / 1024;
            uint64_t off = (lin % 1024) * 4;
            if (page >= u.nframes) return;
            uint64_t chunk = 4096 - off;
            if (chunk > npix * 4) chunk = npix * 4;
            uint8_t *src = (uint8_t *)paging::phys_to_virt(u.frames[page]) + off;
            for (uint64_t b = 0; b < chunk; b++) dst[b] = src[b];
            dst += chunk;
            lin += chunk / 4;
            npix -= chunk / 4;
        }
    }
}

Rect cap_rect(const Window &wd, int idx) {
    return {wd.x + (int)wd.w - 1 - (idx + 1) * kCapW, wd.y + 1, kCapW, kTitleH - 1};
}

void draw_close_glyph(int cx, int cy, uint32_t c) {
    for (int k = 0; k < 10; k++) {
        put(s_scr, cx + k, cy + k, c);
        put(s_scr, cx + 9 - k, cy + k, c);
        put(s_scr, cx + k + 1, cy + k, c, 70);
        put(s_scr, cx + k - 1, cy + k, c, 70);
        put(s_scr, cx + 10 - k, cy + k, c, 70);
        put(s_scr, cx + 8 - k, cy + k, c, 70);
    }
}

void paint_window(Window &wd) {
    const bool foc = (&wd == s_focused);
    const int x = wd.x, y = wd.y, w = (int)wd.w, h = (int)wd.h;
    CornerBuf cb;
    snap_corners(x, y, w, h, kWinR, cb);
    const uint32_t bar = foc ? pal::TITLE_ACTIVE : pal::TITLE_IDLE;
    const uint32_t edge = foc ? pal::EDGE_ACTIVE : pal::EDGE_IDLE;
    fill_rect(s_scr, x, y, w, h, edge);
    fill_rect(s_scr, x + 1, y + 1, w - 2, kTitleH - 1, bar);
    if (kBorder > 1) {
        fill_rect(s_scr, x + 1, y + kTitleH, kBorder - 1, h - kTitleH - 1, bar);
        fill_rect(s_scr, x + w - kBorder, y + kTitleH, kBorder - 1, h - kTitleH - 1, bar);
        fill_rect(s_scr, x + 1, y + h - kBorder, w - 2, kBorder - 1, bar);
    }
    int text_x = x + 12;
    if (glyph(win_icon(wd))) {
        draw_icon(s_scr, win_icon(wd), x + 10, y + (kTitleH - 24) / 2);
        text_x = x + 42;
    }
    text(s_scr, wd.title, text_x, vcenter(y, kTitleH), foc ? pal::TEXT : pal::TEXT_DIM);
    const Rect mn = cap_rect(wd, 1);
    if (hovered(mn.x, mn.y, mn.w, mn.h)) fill_rect(s_scr, mn.x, mn.y, mn.w, mn.h, pal::WHITE, 24);
    fill_rect(s_scr, mn.x + (mn.w - 10) / 2, mn.y + mn.h / 2, 10, 1, foc ? pal::TEXT : pal::TEXT_DIM);
    const Rect cl = cap_rect(wd, 0);
    const bool cl_hot = hovered(cl.x, cl.y, cl.w, cl.h);
    if (cl_hot) fill_rect(s_scr, cl.x, cl.y, cl.w, cl.h, pal::DANGER);
    draw_close_glyph(cl.x + (cl.w - 10) / 2, cl.y + (cl.h - 10) / 2,
                     cl_hot ? pal::WHITE : (foc ? pal::TEXT : pal::TEXT_DIM));
    if (wd.kind == WK_USER && wd.uw >= 0 && (unsigned)wd.uw < UW_MAX && s_uw[(unsigned)wd.uw].used) {
        UWSlot &u = s_uw[(unsigned)wd.uw];
        blit_user(u, x + kBorder, y + kTitleH);
        UWHead *hd = uw_head(u);
        if (hd) hd->dirty = 0;
    } else {
        blit(wd.px, content_w(wd), content_h(wd), content_w(wd), x + kBorder, y + kTitleH);
    }
    apply_corners(x, y, w, h, kWinR, cb, edge);
}

void append(char *dst, uint64_t &pos, const char *s) {
    while (*s) dst[pos++] = *s++;
}
void append_num(char *dst, uint64_t &pos, unsigned long long v) {
    char t[32];
    util::to_str(v, t);
    append(dst, pos, t);
}
void append_two(char *dst, uint64_t &pos, uint64_t v) {
    dst[pos++] = (char)('0' + (v / 10) % 10);
    dst[pos++] = (char)('0' + v % 10);
}
void append_size(char *dst, uint64_t &pos, size_t sz) {
    if (sz < 1024) {
        append_num(dst, pos, sz);
        append(dst, pos, " B");
        return;
    }
    append_num(dst, pos, sz / 1024);
    dst[pos++] = '.';
    append_num(dst, pos, (sz % 1024) * 10 / 1024);
    append(dst, pos, " KB");
}

bool starts_with(const char *s, const char *pre) {
    while (*pre) {
        if (*s++ != *pre++) return false;
    }
    return true;
}
bool ends_with(const char *s, const char *suf) {
    const size_t n = util::strlen(s), m = util::strlen(suf);
    if (m > n) return false;
    return !util::strcmp(s + n - m, suf);
}

void open_window(WinKind k) {
    Window &wd = s_win[k];
    wd.visible = true;
    wd.minimized = false;
    s_focused = &wd;
    s_dirty = true;
}

void focus_window(Window &wd) {
    wd.visible = true;
    wd.minimized = false;
    s_focused = &wd;
    s_dirty = true;
}

void focus_next_visible(Window *closed = nullptr) {
    s_focused = nullptr;
    for (int k = 0; k < WK_N; k++) {
        if (&s_win[k] != closed && shown(s_win[k])) {
            s_focused = &s_win[k];
            return;
        }
    }
    for (unsigned k = 0; k < UW_MAX; k++) {
        if (&s_uwin[k] != closed && shown(s_uwin[k])) {
            s_focused = &s_uwin[k];
            return;
        }
    }
}

constexpr int SLOT_W = 46, SLOT_MAX = 10;
const char *kDate = "12/10/2026";

// Defined with the desktop icons; the taskbar launches through it.
void shortcut_activate(int i, uint32_t where);

int task_y0() { return s_scr.h - kTaskH; }

// An Icon= name is a request, not a promise: a typo or a missing asset must not
// leave a blank tile, so fall back to the generic one.
const char *entry_icon(const shortcuts::Entry &e) {
    if (e.icon[0] && icon::find(e.icon)) return e.icon;
    return e.kind == shortcuts::Kind::Link ? "folder-icon" : "basic-file";
}

// Does any taskbar shortcut launch this program? Used to decide which open
// windows still need their own slot.
bool task_claimed(const char *prog) {
    if (!prog[0]) return false;
    const unsigned pins = shortcuts::count_showing(shortcuts::kShowTaskbar);
    for (unsigned i = 0; i < pins; i++) {
        const shortcuts::Entry *e = shortcuts::at_showing(i, shortcuts::kShowTaskbar);
        if (!e || e->kind == shortcuts::Kind::Link || starts_with(e->exec, "choco://")) continue;
        const char *base = e->exec;
        for (const char *q = e->exec; *q; q++)
            if (*q == '/') base = q + 1;
        size_t n = 0;
        while (base[n] && base[n] != ' ' && base[n] != '"') n++;
        if (n != util::strlen(prog)) continue;
        size_t m = 0;
        for (; m < n; m++)
            if (prog[m] != base[m]) break;
        if (m == n) return true;
    }
    return false;
}

// Slots: Start, then the pinned shortcuts, then any open window no shortcut
// owns - an app started from the shell has to stay reachable too. The old code
// mapped every slot to one kernel window, which made all of them light up
// together.
int task_pins() {
    const unsigned n = shortcuts::count_showing(shortcuts::kShowTaskbar);
    return n > (unsigned)(SLOT_MAX - 2) ? SLOT_MAX - 2 : (int)n;
}
int task_orphans() {
    int n = 0;
    for (unsigned i = 0; i < UW_MAX; i++)
        if (s_uwin[i].kind == WK_USER && s_uwin[i].visible && !task_claimed(s_uwin[i].prog)) n++;
    return n;
}
int task_slots() {
    const int n = 1 + task_pins() + task_orphans();
    return n > SLOT_MAX ? SLOT_MAX : n;
}

int slot_x(int i) { return s_scr.w / 2 - (task_slots() * SLOT_W) / 2 + i * SLOT_W; }
Rect slot_rect(int i) { return {slot_x(i) + 2, task_y0() + 3, SLOT_W - 4, kTaskH - 6}; }

// The window this shortcut's program owns right now, if any. A minimized window
// still counts (that is how you get it back); a closed one does not, or the slot
// would read as "running" before the app was ever opened.
Window *entry_window(const shortcuts::Entry &e) {
    if (e.kind == shortcuts::Kind::Link) return nullptr;
    if (starts_with(e.exec, "choco://terminal"))
        return s_win[WK_TERM].visible ? &s_win[WK_TERM] : nullptr;
    if (e.kind == shortcuts::Kind::Link) return nullptr;
    const char *base = e.exec;
    for (const char *q = e.exec; *q; q++)
        if (*q == '/') base = q + 1;
    char prog[24];
    size_t n = 0;
    while (base[n] && base[n] != ' ' && base[n] != '"' && n + 1 < sizeof(prog)) {
        prog[n] = base[n];
        n++;
    }
    prog[n] = 0;
    if (!n) return nullptr;
    for (unsigned i = 0; i < UW_MAX; i++)
        if (s_uwin[i].kind == WK_USER && s_uwin[i].visible && !util::strcmp(s_uwin[i].prog, prog))
            return &s_uwin[i];
    return nullptr;
}

struct TaskTarget {
    const shortcuts::Entry *entry;
    Window *win;
};

TaskTarget task_target(int slot) {
    if (slot <= 0) return {nullptr, nullptr};
    const int pins = task_pins();
    if (slot <= pins) {
        const shortcuts::Entry *e = shortcuts::at_showing((size_t)(slot - 1), shortcuts::kShowTaskbar);
        return {e, e ? entry_window(*e) : nullptr};
    }
    int skip = slot - pins - 1;
    for (unsigned i = 0; i < UW_MAX; i++) {
        if (s_uwin[i].kind != WK_USER || !s_uwin[i].visible || task_claimed(s_uwin[i].prog)) continue;
        if (skip-- == 0) return {nullptr, &s_uwin[i]};
    }
    return {nullptr, nullptr};
}

const char *task_icon(const TaskTarget &t) {
    if (t.entry) return entry_icon(*t.entry);
    if (t.win) return win_icon(*t.win);
    return "basic-file";
}

int hit_task(int x, int y) {
    const int n = task_slots();
    for (int i = 0; i < n; i++) {
        const Rect r = slot_rect(i);
        if (in_rect(x, y, r.x, r.y, r.w, r.h)) return i;
    }
    return -1;
}

void task_click(int slot) {
    if (slot == 0) { s_menu = !s_menu; s_dirty = true; return; }
    const TaskTarget t = task_target(slot);
    if (t.win) {
        // Windows behaviour: focused and visible means minimize, anything else
        // brings it forward. Never a second copy of a running program.
        if (shown(*t.win) && s_focused == t.win) {
            t.win->minimized = true;
            focus_next_visible(t.win);
        } else {
            focus_window(*t.win);
        }
        s_dirty = true;
        return;
    }
    if (t.entry) shortcut_activate(slot - 1, shortcuts::kShowTaskbar);
}

void draw_start_logo(int x, int y) {
    round_rect(s_scr, x, y, 9, 9, 2, 0xC4B5FD);
    round_rect(s_scr, x + 11, y, 9, 9, 2, 0xA78BFA);
    round_rect(s_scr, x, y + 11, 9, 9, 2, 0x8B5CF6);
    round_rect(s_scr, x + 11, y + 11, 9, 9, 2, 0x7C3AED);
}

void taskbar() {
    const int ty = task_y0();
    fill_rect(s_scr, 0, ty, s_scr.w, kTaskH, pal::TASKBAR, 238);
    fill_rect(s_scr, 0, ty, s_scr.w, 1, pal::TASK_EDGE);
    for (int i = 0; i < task_slots(); i++) {
        const Rect r = slot_rect(i);
        const bool hot = hovered(r.x, r.y, r.w, r.h);
        const TaskTarget t = task_target(i);
        bool running = false, active = false;
        if (i == 0) {
            active = s_menu;
        } else if (t.win) {
            running = true;
            active = shown(*t.win) && s_focused == t.win;
        }
        const unsigned a = (active ? 30u : 0u) + (hot ? 20u : 0u);
        if (a) round_rect(s_scr, r.x, r.y, r.w, r.h, 6, pal::WHITE, a);
        if (i == 0) {
            draw_start_logo(r.x + (r.w - 20) / 2, r.y + (r.h - 20) / 2 - 1);
            continue;
        }
        draw_icon(s_scr, task_icon(t), r.x + (r.w - 24) / 2, r.y + (r.h - 24) / 2 - 2);
        if (running) {
            const int iw = active ? 16 : 6;
            round_rect(s_scr, r.x + (r.w - iw) / 2, r.y + r.h - 5, iw, 3, 1,
                       active ? pal::ACCENT_HI : pal::TEXT_DIM);
        }
    }
    const uint64_t secs = pit::seconds();
    char tb[8];
    uint64_t p = 0;
    append_two(tb, p, secs / 3600);
    tb[p++] = ':';
    append_two(tb, p, (secs / 60) % 60);
    tb[p] = 0;
    const int date_w = text_w(kDate);
    const int right = s_scr.w - 14;
    const int tx = right - date_w;
    const Rect net_slot = {tx - 12 - 24, ty + (kTaskH - 24) / 2, 24, 24};
    const int tray_x = net_slot.x - 8;
    const bool tray_hot = hovered(tray_x, ty + 3, s_scr.w - 6 - tray_x, kTaskH - 6);
    round_rect(s_scr, tray_x, ty + 3, s_scr.w - 6 - tray_x, kTaskH - 6, 6, pal::WHITE,
               tray_hot ? 22 : 10);
    draw_icon(s_scr, glyph("no_wifi_icon") ? "no_wifi_icon" : "wifi_icon", net_slot.x, net_slot.y);
    const int lh = font::line_height();
    const int ly = ty + (kTaskH - 2 * lh) / 2;
    text_right(s_scr, tb, right, ly, pal::TEXT);
    text_right(s_scr, kDate, right, ly + lh, pal::TEXT_DIM);
}

constexpr int MENU_W = 420, MENU_H = 400;
constexpr int PIN_PER_ROW = 4, PIN_MAX = 8, PIN_W = 84, PIN_H = 68;
constexpr int REC_N = 4, REC_W = 190, REC_H = 38;

// The Start menu's "Pinned" row is the same data as the desktop: .desktop
// entries that ask for StartMenu (kShowMenu). Nothing here names an app.
int pins_showing() {
    const size_t n = shortcuts::count_showing(shortcuts::kShowMenu);
    return (int)(n > (size_t)PIN_MAX ? (size_t)PIN_MAX : n);
}

int menu_x() { return (s_scr.w - MENU_W) / 2; }
int menu_y() {
    const int y = task_y0() - 12 - MENU_H;
    return y < 8 ? 8 : y;
}
int pin_x(int i) {
    return menu_x() + (MENU_W - PIN_PER_ROW * PIN_W) / 2 + (i % PIN_PER_ROW) * PIN_W;
}
int pin_y(int i) { return menu_y() + 94 + (i / PIN_PER_ROW) * (PIN_H + 14); }
int rec_x(int i) { return menu_x() + 20 + (i % 2) * REC_W; }
int rec_y(int i) { return menu_y() + 258 + (i / 2) * 42; }
int foot_y() { return menu_y() + MENU_H - 52; }
Rect shutdown_rect() { return {menu_x() + MENU_W - 20 - 84, foot_y() + 10, 84, 32}; }
Rect reboot_rect() { return {shutdown_rect().x - 8 - 68, foot_y() + 10, 68, 32}; }

const char *file_base(const char *path) {
    const char *base = path;
    for (const char *q = path; *q; q++)
        if (*q == '/') base = q + 1;
    return base;
}

void menu_button(const Rect &r, const char *label) {
    const bool hot = hovered(r.x, r.y, r.w, r.h);
    round_rect(s_scr, r.x, r.y, r.w, r.h, 6, hot ? pal::TILE_HI : pal::TILE);
    text_center(s_scr, label, r.x + r.w / 2, vcenter(r.y, r.h), pal::TEXT);
}

void start_menu() {
    const int mx = menu_x(), my = menu_y();
    fill_rect(s_scr, mx, my, MENU_W, MENU_H, pal::PANEL, 246);
    const int fy = foot_y();
    fill_rect(s_scr, mx, fy, MENU_W, 52, pal::PANEL_FOOT, 250);
    fill_rect(s_scr, mx, fy, MENU_W, 1, pal::PANEL_EDGE);
    fill_rect(s_scr, mx, my, MENU_W, 1, pal::PANEL_EDGE);
    fill_rect(s_scr, mx, my + MENU_H - 1, MENU_W, 1, pal::PANEL_EDGE);
    fill_rect(s_scr, mx, my, 1, MENU_H, pal::PANEL_EDGE);
    fill_rect(s_scr, mx + MENU_W - 1, my, 1, MENU_H, pal::PANEL_EDGE);

    round_rect(s_scr, mx + 20, my + 16, MENU_W - 40, 36, 10, pal::FIELD_EDGE);
    round_rect(s_scr, mx + 21, my + 17, MENU_W - 42, 34, 9, pal::FIELD);
    draw_icon(s_scr, "search_icon", mx + 30, my + 22);
    text(s_scr, "Type here to search", mx + 62, my + 30, pal::TEXT_DIM);

    text(s_scr, "Pinned", mx + 24, my + 70, pal::TEXT);
    const int npins = pins_showing();
    for (int i = 0; i < npins; i++) {
        const shortcuts::Entry *e = shortcuts::at_showing((size_t)i, shortcuts::kShowMenu);
        if (!e) continue;
        const int cx = pin_x(i), cy = pin_y(i);
        const bool hot = hovered(cx, cy, PIN_W, PIN_H);
        if (hot) round_rect(s_scr, cx + 2, cy - 2, PIN_W - 4, PIN_H, 8, pal::WHITE, 16);
        round_rect(s_scr, cx + (PIN_W - 40) / 2, cy + 2, 40, 40, 8, hot ? pal::TILE_HI : pal::TILE);
        const int ix = cx + (PIN_W - 24) / 2, iy = cy + 10;
        draw_icon(s_scr, entry_icon(*e), ix, iy);
        // A shortcut whose target is gone stays visible but faded, so the user
        // can see why the icon does nothing rather than blame the pointer.
        if (!e->valid) round_rect(s_scr, ix - 1, iy - 1, 26, 26, 4, hot ? pal::TILE_HI : pal::TILE, 175);
        text_center(s_scr, e->name, cx + PIN_W / 2, cy + 48,
                    e->valid ? pal::TEXT : pal::TEXT_DIM);
    }
    if (!npins) text(s_scr, "No shortcuts yet - create one with 'lnk'", mx + 24, my + 104, pal::TEXT_DIM);

    text(s_scr, "Recent files", mx + 24, my + 234, pal::TEXT);
    const size_t files = fs::count();
    if (!files) text(s_scr, "No files yet", mx + 24, my + 206, pal::TEXT_DIM);
    for (int i = 0; i < REC_N && (size_t)i < files; i++) {
        const char *nm = nullptr;
        size_t sz = 0;
        if (!fs::stat((size_t)i, &nm, &sz)) continue;
        const int cx = rec_x(i), cy = rec_y(i);
        const bool hot = hovered(cx, cy, REC_W, REC_H);
        const bool program = starts_with(nm, "bin/");
        if (hot) round_rect(s_scr, cx, cy, REC_W, REC_H, 6, pal::WHITE, 18);
        round_rect(s_scr, cx + 4, cy + 4, 30, 30, 6, pal::TILE);
        draw_icon(s_scr, program ? "code_icon" : "open_file", cx + 7, cy + 7);
        char title[20];
        const char *base = file_base(nm);
        int n = 0;
        while (base[n] && n < 16) {
            title[n] = base[n];
            n++;
        }
        title[n] = 0;
        char sub[40];
        uint64_t p = 0;
        append(sub, p, program ? "Program  " : "Document  ");
        append_size(sub, p, sz);
        sub[p] = 0;
        text(s_scr, title, cx + 42, cy + 2, pal::TEXT);
        text(s_scr, sub, cx + 42, cy + 20, pal::TEXT_DIM);
    }

    round_rect(s_scr, mx + 24, fy + 14, 24, 24, 12, pal::ACCENT_LO);
    text_center(s_scr, "C", mx + 36, vcenter(fy + 14, 24), pal::WHITE);
    text(s_scr, "choco", mx + 58, vcenter(fy + 14, 24), pal::TEXT);
    menu_button(reboot_rect(), "Reboot");
    menu_button(shutdown_rect(), "Shutdown");
}

int hit_menu_power(int x, int y) {
    const Rect rb = reboot_rect(), sd = shutdown_rect();
    if (in_rect(x, y, rb.x, rb.y, rb.w, rb.h)) return 1;
    if (in_rect(x, y, sd.x, sd.y, sd.w, sd.h)) return 2;
    return 0;
}
int hit_menu_pin(int x, int y) {
    for (int i = 0; i < pins_showing(); i++)
        if (in_rect(x, y, pin_x(i), pin_y(i), PIN_W, PIN_H)) return i;
    return -1;
}
int hit_menu_recent(int x, int y) {
    for (int i = 0; i < REC_N && (size_t)i < fs::count(); i++)
        if (in_rect(x, y, rec_x(i), rec_y(i), REC_W, REC_H)) return i;
    return -1;
}
bool hit_menu(int x, int y) { return in_rect(x, y, menu_x(), menu_y(), MENU_W, MENU_H); }

// The desktop is a view of the shortcut table: entries that ask for Desktop.
// Count and order come from the filesystem, never from a list in this file.
const shortcuts::Entry *desk_at(int i) {
    return shortcuts::at_showing((size_t)i, shortcuts::kShowDesktop);
}
int desk_showing() {
    return (int)shortcuts::count_showing(shortcuts::kShowDesktop);
}
// Icons flow down a column and wrap into the next one, like Windows.
int desk_rows() {
    const int h = task_y0() - 24;
    const int r = h / (kCellH + 6);
    return r < 1 ? 1 : r;
}
int desk_x(int i) { return 12 + (i / desk_rows()) * (kCellW + 4); }
int desk_y(int i) { return 12 + (i % desk_rows()) * (kCellH + 6); }

void desktop_icons() {
    const int n = desk_showing();
    for (int i = 0; i < n; i++) {
        const shortcuts::Entry *e = desk_at(i);
        if (!e) continue;
        const int cx0 = desk_x(i), cy0 = desk_y(i);
        const bool sel = i == s_sel_icon;
        const bool hot = hovered(cx0, cy0, kCellW, kCellH);
        if (sel || hot) {
            round_rect(s_scr, cx0, cy0, kCellW, kCellH, 6, pal::WHITE, sel ? 44 : 22);
            if (sel) round_rect(s_scr, cx0, cy0, kCellW, kCellH, 6, pal::ACCENT, 40);
        }
        const int ix = cx0 + (kCellW - 24) / 2, iy = cy0 + 8;
        draw_icon(s_scr, entry_icon(*e), ix, iy);
        // Target gone: keep the shortcut (so the user can see and remove it)
        // and mark it, instead of silently pretending it is not there.
        if (!e->valid) fill_rect(s_scr, ix + 19, iy - 1, 6, 6, pal::DANGER);
        const char *nm = e->name;
        const int lx = cx0 + (kCellW - text_w(nm)) / 2, ly = iy + 24 + 8;
        text(s_scr, nm, lx + 1, ly + 1, pal::BLACK, 1);
        text(s_scr, nm, lx, ly, e->valid ? pal::WHITE : pal::TEXT_DIM);
    }
}
int hit_desk(int x, int y) {
    const int n = desk_showing();
    for (int i = 0; i < n; i++)
        if (in_rect(x, y, desk_x(i), desk_y(i), kCellW, kCellH)) return i;
    return -1;
}

// Double-headed arrow for the resize bands: axis follows the edge grabbed.
void draw_resize_cursor(int x, int y, unsigned zone) {
    auto line = [&](int x0, int y0, int x1, int y1) {
        const int dx = x1 - x0, dy = y1 - y0;
        int n = dx < 0 ? -dx : dx;
        if ((dy < 0 ? -dy : dy) > n) n = dy < 0 ? -dy : dy;
        if (n == 0) { put(s_scr, x0, y0, pal::WHITE); return; }
        for (int i = 0; i <= n; i++)
            put(s_scr, x0 + (dx * i) / n, y0 + (dy * i) / n, pal::WHITE);
    };
    const bool lr = (zone & (EZ_L | EZ_R)) != 0, tb = (zone & (EZ_T | EZ_B)) != 0;
    if (lr && !tb) {
        line(x - 6, y, x + 6, y);
        line(x - 6, y, x - 3, y - 3); line(x - 6, y, x - 3, y + 3);
        line(x + 6, y, x + 3, y - 3); line(x + 6, y, x + 3, y + 3);
    } else if (tb && !lr) {
        line(x, y - 6, x, y + 6);
        line(x, y - 6, x - 3, y - 3); line(x, y - 6, x + 3, y - 3);
        line(x, y + 6, x - 3, y + 3); line(x, y + 6, x + 3, y + 3);
    } else {
        const int s = ((zone & EZ_L) && (zone & EZ_B)) ||
                      ((zone & EZ_R) && (zone & EZ_T)) ? -1 : 1;
        line(x - 6 * s, y - 6 * s, x + 6 * s, y + 6 * s);
        line(x + 6 * s, y + 6 * s, x + 2 * s, y + 6 * s);
        line(x + 6 * s, y + 6 * s, x + 6 * s, y + 2 * s);
        line(x - 6 * s, y - 6 * s, x - 2 * s, y - 6 * s);
        line(x - 6 * s, y - 6 * s, x - 6 * s, y - 2 * s);
    }
}

// Copy the pixels under the pointer so a later update can erase the glyph
// without recomposing the scene; restore puts them back at the saved position.
void cursor_save(int x, int y) {
    const int bx = x - kCurPad, by = y - kCurPad;
    for (int r = 0; r < kCurBox; r++)
        for (int c = 0; c < kCurBox; c++) {
            const int xx = bx + c, yy = by + r;
            s_curbg[r * kCurBox + c] =
                (xx < 0 || yy < 0 || xx >= s_scr.w || yy >= s_scr.h)
                    ? 0u
                    : s_scr.d[(uint64_t)yy * s_scr.pitch + (uint64_t)xx];
        }
    s_curx = x; s_cury = y; s_curon = true;
}

void cursor_restore() {
    if (!s_curon) return;
    const int bx = s_curx - kCurPad, by = s_cury - kCurPad;
    for (int r = 0; r < kCurBox; r++)
        for (int c = 0; c < kCurBox; c++) {
            const int xx = bx + c, yy = by + r;
            if (xx < 0 || yy < 0 || xx >= s_scr.w || yy >= s_scr.h) continue;
            s_scr.d[(uint64_t)yy * s_scr.pitch + (uint64_t)xx] = s_curbg[r * kCurBox + c];
        }
    mark_dirty(bx, by, kCurBox, kCurBox);
    s_curon = false;
}

void draw_cursor(int x, int y) {
    cursor_save(x, y);
    if (s_cursor_zone != EZ_NONE) {
        draw_resize_cursor(x, y, s_cursor_zone);
        mark_dirty(x - kCurPad, y - kCurPad, kCurBox, kCurBox);
        return;
    }
    static const char *kRows[] = {
        "X           ", "XX          ", "XoX         ", "XooX        ", "XoooX       ",
        "XooooX      ", "XoooooX     ", "XooooooX    ", "XoooooooX   ", "XooooooooX  ",
        "XoooooXXXXX ", "XooXooX     ", "XoX XooX    ", "XX  XooX    ", "X    XooX   ",
        "     XooX   ", "      XX    "};
    for (int r = 0; r < 17; r++)
        for (int c = 0; kRows[r][c]; c++) {
            if (kRows[r][c] == '.' || kRows[r][c] == ' ') continue;
            if (kRows[r][c] == 'X') put(s_scr, x + c, y + r, pal::BLACK);
            else put(s_scr, x + c, y + r, pal::WHITE);
        }
    // Mark cursor region as dirty for partial present
    mark_dirty(x - kCurPad, y - kCurPad, kCurBox, kCurBox);
}

// Blit the back buffer to the framebuffer. A full copy when the whole scene
// changed, otherwise only the dirty rectangles (clamped to the screen).
void present() {
    if (!s_back || !s_fb) { s_dirty = false; g_dirty_count = 0; return; }
    if (s_dirty || g_dirty_count == 0) {
        for (int y = 0; y < s_scr.h; y++) {
            memcpy(&s_fb[(size_t)y * s_fb_pitch],
                   &s_back[(size_t)y * s_scr.w],
                   (size_t)s_scr.w * 4);
        }
    } else {
        for (int i = 0; i < g_dirty_count; i++) {
            DirtyRect &r = g_dirty[i];
            if (!r.used) continue;
            int x0 = r.x < 0 ? 0 : r.x;
            int x1 = r.x + r.w;
            if (x1 > s_scr.w) x1 = s_scr.w;
            if (x0 >= x1) continue;
            const size_t bytes = (size_t)(x1 - x0) * 4;
            for (int y = 0; y < r.h; y++) {
                int sy = r.y + y;
                if (sy < 0 || sy >= s_scr.h) continue;
                memcpy(&s_fb[(size_t)sy * s_fb_pitch + x0],
                       &s_back[(size_t)sy * s_scr.w + x0], bytes);
            }
        }
    }
    s_dirty = false;
    g_dirty_count = 0;
}

void paint() {
    if (s_wall) blit(s_wall, (uint64_t)s_scr.w, (uint64_t)s_scr.h, (uint64_t)s_scr.w, 0, 0);
    else fill_rect(s_scr, 0, 0, s_scr.w, s_scr.h, WIN_BG);
    desktop_icons();
    Window *ord[3 + UW_MAX];
    int n = 0;
    for (int k = 0; k < WK_N; k++)
        if (shown(s_win[k]) && &s_win[k] != s_focused) ord[n++] = &s_win[k];
    for (unsigned k = 0; k < UW_MAX; k++)
        if (shown(s_uwin[k]) && &s_uwin[k] != s_focused) ord[n++] = &s_uwin[k];
    if (s_focused && shown(*s_focused)) ord[n++] = s_focused;
    for (int i = 0; i < n; i++) {
        drop_shadow(ord[i]->x, ord[i]->y, ord[i]->w, ord[i]->h, ord[i] == s_focused ? 256 : 150);
        paint_window(*ord[i]);
    }
    if (s_menu) {
        drop_shadow(menu_x(), menu_y(), MENU_W, MENU_H, 256);
        CornerBuf mcb;
        snap_corners(menu_x(), menu_y(), MENU_W, MENU_H, kMenuR, mcb);
        start_menu();
        apply_corners(menu_x(), menu_y(), MENU_W, MENU_H, kMenuR, mcb, pal::PANEL_EDGE);
    }
    taskbar();
    // The scene was just recomposed, so any previously drawn pointer is gone:
    // forget the save-under before painting the pointer again.
    s_curon = false;
    if (mouse::present()) draw_cursor(mouse::x(), mouse::y());
    present();
}

bool hit_x(const Window &wd, int mx, int my) {
    const Rect r = cap_rect(wd, 0);
    return in_rect(mx, my, r.x, r.y, r.w, r.h);
}
bool hit_min(const Window &wd, int mx, int my) {
    const Rect r = cap_rect(wd, 1);
    return in_rect(mx, my, r.x, r.y, r.w, r.h);
}
bool hit_title(const Window &wd, int mx, int my) {
    return in_rect(mx, my, wd.x, wd.y, (int)wd.w, kTitleH);
}
bool hit_win(const Window &wd, int mx, int my) {
    return shown(wd) && in_rect(mx, my, wd.x, wd.y, (int)wd.w, (int)wd.h);
}

// --- resize / maximize -----------------------------------------------------
// All windows resize. User windows never reallocate: their shared buffer is
// allocated at full capacity on creation, so a resize only changes the visible
// w/h and tells the app (UEV_RESIZE) to lay out again.
bool resizable(const Window &) { return true; }

unsigned resize_zones(const Window &wd, int mx, int my) {
    if (!resizable(wd) || !shown(wd)) return EZ_NONE;
    const int l = wd.x, t = wd.y;
    const int r = wd.x + (int)wd.w - 1, b = wd.y + (int)wd.h - 1;
    if (mx < l - kGrab || mx > r + kGrab || my < t - kGrab || my > b + kGrab) return EZ_NONE;
    unsigned z = EZ_NONE;
    if (mx <= l + kGrab) z |= EZ_L;
    if (mx >= r - kGrab) z |= EZ_R;
    if (my <= t + kGrab) z |= EZ_T;
    if (my >= b - kGrab) z |= EZ_B;
    return z;
}

// Pointer feedback: which resize band (if any) is under the cursor.
void update_cursor_zone(int mx, int my) {
    unsigned z = EZ_NONE;
    if (s_focused && shown(*s_focused)) z = resize_zones(*s_focused, mx, my);
    for (int k = 0; k < WK_N && z == EZ_NONE; k++)
        if (shown(s_win[k])) z = resize_zones(s_win[k], mx, my);
    s_cursor_zone = z;
}

// New outer rect for a drag of (dx,dy) on the given edges, clamped to the
// minimum size and to the screen.
void resize_rect(int &x, int &y, int &w, int &h, unsigned zone, int dx, int dy) {
    if (zone & EZ_R) w += dx;
    if (zone & EZ_B) h += dy;
    if (zone & EZ_L) { x += dx; w -= dx; }
    if (zone & EZ_T) { y += dy; h -= dy; }
    if (w < kMinW) { if (zone & EZ_L) x -= kMinW - w; w = kMinW; }
    if (h < kMinH) { if (zone & EZ_T) y -= kMinH - h; h = kMinH; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < kMinW) w = kMinW;
    if (h < kMinH) h = kMinH;
    if (x + w > s_scr.w) w = s_scr.w - x;
    if (y + h > s_scr.h) h = s_scr.h - y;
}

// Give a window a new outer size: reallocate the content bitmap and keep the
// pixels that still fit, so a resize does not blank the window.
bool win_resize(Window &wd, int nx, int ny, int nw, int nh) {
    if (!resizable(wd)) return false;
    const int ncw = nw - 2 * kBorder;
    const int nch = nh - kTitleH - kBorder;
    if (ncw <= 0 || nch <= 0) return false;
    if ((int)wd.w == nw && (int)wd.h == nh && wd.x == nx && wd.y == ny) return false;
    if (wd.kind == WK_USER && wd.uw >= 0 && (unsigned)wd.uw < UW_MAX) {
        UWSlot &u = s_uw[(unsigned)wd.uw];
        if (!u.used) return false;
        uint64_t cw = (uint64_t)ncw, ch = (uint64_t)nch;
        if (cw > UW_MAX_W) cw = UW_MAX_W;   // capacity is fixed at creation
        if (ch > UW_MAX_H) ch = UW_MAX_H;
        u.w = cw;
        u.h = ch;
        wd.x = nx;
        wd.y = ny;
        wd.w = cw + 2 * (uint64_t)kBorder;
        wd.h = ch + (uint64_t)kTitleH + kBorder;
        UWHead *hd = uw_head(u);
        if (hd && hd->magic == UW_MAGIC) {
            hd->w = cw;
            hd->h = ch;
            uw_push(u, UEV_RESIZE, cw, ch, 0);
        }
        s_dirty = true;
        return true;
    }
    uint32_t *np = nullptr;
    if (wd.px) {
        np = (uint32_t *)heap::alloc((size_t)ncw * (size_t)nch * 4);
        if (!np) return false;  // out of memory: keep the current size
        const int ocw = content_w(wd), och = content_h(wd);
        const int cx = ncw < ocw ? ncw : ocw;
        const int cy = nch < och ? nch : och;
        for (int j = 0; j < cy; j++)
            util::memcpy(np + (size_t)j * ncw, wd.px + (size_t)j * ocw, (size_t)cx * 4);
    }
    int curx = 0, cury = 0;
    if (wd.is_term) console::get_cursor(curx, cury);
    if (wd.px) heap::free(wd.px);
    wd.px = np;
    wd.x = nx; wd.y = ny; wd.w = (uint64_t)nw; wd.h = (uint64_t)nh;
    if (wd.is_term && np) {
        console::set_surface(np, (uint64_t)ncw, (uint64_t)nch, (uint64_t)ncw);
        console::set_cursor(curx, cury);
    }
    s_dirty = true;
    return true;
}

void win_maximize(Window &wd) {
    if (!resizable(wd)) return;
    if (!wd.maximized) {
        wd.rx = wd.x; wd.ry = wd.y; wd.rw = wd.w; wd.rh = wd.h;
        win_resize(wd, 0, 0, s_scr.w, task_y0());
        wd.maximized = true;
    } else {
        win_resize(wd, wd.rx, wd.ry, (int)wd.rw, (int)wd.rh);
        wd.maximized = false;
    }
    s_dirty = true;
}

// Aero snap: half the screen at the left/right edge, maximized at the top.
void win_snap(Window &wd, int side) {
    if (!resizable(wd) || side == 0) return;
    if (!wd.maximized) { wd.rx = wd.x; wd.ry = wd.y; wd.rw = wd.w; wd.rh = wd.h; }
    const int work_h = task_y0(), half = s_scr.w / 2;
    if (side == 3) { win_resize(wd, 0, 0, s_scr.w, work_h); wd.maximized = true; }
    else if (side == 1) { win_resize(wd, 0, 0, half, work_h); wd.maximized = false; }
    else { win_resize(wd, half, 0, s_scr.w - half, work_h); wd.maximized = false; }
    s_dirty = true;
}

void open_file_entry(const char *path) {
    if (ends_with(path, ".txt")) {
        const void *d = nullptr;
        size_t sz = 0;
        if (!fs::read(path, &d, &sz)) return;
        open_window(WK_TERM);
        const char *s = (const char *)d;
        for (size_t i = 0; i < sz; i++) console::put_char(s[i], 0xEAEAEA, WIN_BG);
        if (sz && s[sz - 1] != '\n') console::put_char('\n', 0xEAEAEA, WIN_BG);
    } else {
        open_window(WK_TERM);
        proc::spawn_job(path, 0, nullptr, nullptr);
    }
}

void open_entry(size_t index) {
    const char *nm = nullptr;
    size_t sz = 0;
    if (!fs::stat(index, &nm, &sz)) return;
    char path[110];
    uint64_t q = 0;
    while (nm[q] && q < sizeof(path) - 1) {
        path[q] = nm[q];
        q++;
    }
    path[q] = 0;
    open_file_entry(path);
}

// choco:// verbs: the few things that are still kernel windows.
bool shortcut_verb(const char *verb) {
    if (util::strcmp(verb, "terminal") == 0) {
        open_window(WK_TERM);
        s_dirty = true;
        return true;
    }
    return false;
}

// Type=Link: a folder opens in the Explorer, a file keeps the old behaviour
// (text prints into the terminal, anything else is run).
bool shortcut_open_path(const char *path) {
    if (!path || !path[0]) return false;
    if (fs::writable_is_dir(path)) {
        char arg[160];
        size_t n = 0;
        for (; path[n] && n + 1 < sizeof(arg); n++) arg[n] = path[n];
        arg[n] = 0;
        char *av[] = {arg};
        return proc::spawn_job("bin/fileman", 1, av, nullptr) >= 0;
    }
    open_file_entry(path);
    return true;
}

// Clicking an icon asks the shortcut table what to do. This code names no
// application: the .desktop file decides (kernel/proc/shortcuts.cpp).
void shortcut_activate(int i, uint32_t where) {
    s_menu = false;
    // Launching no longer blocks: a job is spawned and this returns at once, so
    // the click that opened it is not swallowed and a second click is not
    // needed. A re-entrancy guard is unnecessary because nothing here pumps
    // the desktop loop synchronously any more.
    shortcuts::activate(shortcuts::at_showing((size_t)i, where));
    s_dirty = true;
}

void do_reboot() {
    cpu::outb(0x64, 0xFE);
    for (;;) {
        asm volatile("hlt");
    }
}

[[noreturn]] void do_shutdown() {
    uint16_t value = 0x2000;
    asm volatile("outw %0, %1" : : "a"(value), "Nd"((uint16_t)0x604));
    for (;;) asm volatile("hlt");
}

} // namespace

void poweroff() { do_shutdown(); }

long user_create(uint64_t w, uint64_t h, const char *title, uint64_t *addr_out) {
    // Caller-scoped: any user process may own a window, whether it was launched
    // from the console (foreground) or as a detached job. The root must be the
    // one currently live, or the frames would be mapped into a foreign space.
    uint64_t root = proc::proc_root();
    if (!root || root != paging::read_cr3()) return -1;
    if (w == 0 || h == 0 || w > UW_MAX_W || h > UW_MAX_H) return -22;
    if (!title) return -14;
    for (unsigned i = 0; i < 40; i++) {
        if (!syscall::user_ok((uint64_t)title + i, 1)) return -14;
        if (title[i] == 0) break;
        if (i == 39) return -22;
    }
    int slot = -1, cell = -1;
    for (unsigned i = 0; i < UW_MAX; i++)
        if (!s_uw[i].used) {
            slot = (int)i;
            break;
        }
    for (unsigned i = 0; i < UW_MAX; i++)
        if (!s_uwin[i].visible && s_uwin[i].kind != WK_USER) {
            cell = (int)i;
            break;
        }
    if (cell < 0)
        for (unsigned i = 0; i < UW_MAX; i++)
            if (s_uwin[i].kind == WK_USER && !s_uwin[i].visible) {
                cell = (int)i;
                break;
            }
    if (slot < 0 || cell < 0) return -24;
    uint64_t total = UW_PIX_OFF + UW_CAP_BYTES;
    uint64_t nframes = (total + 4095) / 4096;
    // One VA slice per slot: closing a window frees its range for reuse. The old
    // monotonic cursor leaked ~1.2 MiB per window and wedged after ~6 opens.
    uint64_t base = UW_BASE + (uint64_t)slot * nframes * 4096;
    if (base + nframes * 4096 > UW_BASE + UW_AREA) return -12;
    uint64_t *frames = (uint64_t *)heap::alloc(sizeof(uint64_t) * (size_t)nframes);
    if (!frames) return -12;
    uint64_t mapped = 0;
    for (uint64_t f = 0; f < nframes; f++) {
        uint64_t ph = pmm::alloc_zero();
        if (!ph) break;
        if (!paging::map_as(root, base + f * 4096, ph,
                            paging::F_PRESENT | paging::F_RW | paging::F_USER)) {
            pmm::free(ph);
            break;
        }
        frames[mapped++] = ph;
    }
    if (mapped != nframes) {
        for (uint64_t f = 0; f < mapped; f++) {
            paging::unmap_as(root, base + f * 4096);
            pmm::free(frames[f]);
        }
        heap::free(frames);
        return -12;
    }
    UWSlot &u = s_uw[(unsigned)slot];
    u.used = true;
    u.root = root;
    u.uaddr = base;
    u.w = w;
    u.h = h;
    u.nframes = nframes;
    u.pitch = UW_MAX_W;
    u.frames = frames;
    u.pid = proc::proc_pid();
    UWHead *hd = uw_head(u);
    hd->magic = UW_MAGIC;
    hd->w = w;
    hd->h = h;
    hd->pitch = UW_MAX_W;
    Window &wd = s_uwin[(unsigned)cell];
    wd.x = 120 + slot * 40;
    wd.y = 110 + slot * 30;
    wd.w = w + 2 * (uint64_t)kBorder;
    wd.h = h + (uint64_t)(kTitleH + kBorder);
    // Zero the reused cell first: a short title after a longer one must not
    // leave the previous occupant's tail, or the taskbar mis-matches the window.
    util::memset(wd.title, 0, sizeof(wd.title));
    for (unsigned i = 0; i < sizeof(wd.title) - 1 && title[i]; i++) wd.title[i] = title[i];
    // Which program owns this window: the taskbar matches shortcuts against it,
    // so clicking a pinned icon focuses the running app instead of launching a
    // second copy.
    util::memset(wd.prog, 0, sizeof(wd.prog));
    const char *comm = proc::proc_comm();
    for (unsigned i = 0; i < sizeof(wd.prog) - 1 && comm && comm[i]; i++) wd.prog[i] = comm[i];
    wd.px = nullptr;
    wd.visible = true;
    wd.minimized = false;
    wd.maximized = false;
    wd.is_term = false;
    wd.kind = WK_USER;
    wd.uw = slot;
    u.win = &wd;
    s_focused = &wd;
    if (addr_out) *addr_out = base;
    s_dirty = true;
    return 0;
}

long user_resize(uint64_t uaddr, uint64_t w, uint64_t h) {
    uint64_t root = proc::proc_root();
    if (!root) return -1;
    UWSlot *u = uw_find(root, uaddr);
    if (!u || !u->win) return -9;
    if (w == 0 || h == 0 || w > UW_MAX_W || h > UW_MAX_H) return -22;
    if (!win_resize(*u->win, u->win->x, u->win->y,
                    (int)(w + 2 * (uint64_t)kBorder),
                    (int)(h + (uint64_t)kTitleH + kBorder)))
        return -22;
    s_dirty = true;
    return 0;
}

long user_present(uint64_t uaddr) {
    uint64_t root = proc::proc_root();
    if (!root) return -1;
    UWSlot *u = uw_find(root, uaddr);
    if (!u) return -9;
    UWHead *hd = uw_head(*u);
    if (!hd || hd->magic != UW_MAGIC) return -9;
    hd->dirty = 1;
    s_dirty = true;
    return 0;
}

long user_close(uint64_t uaddr) {
    uint64_t root = proc::proc_root();
    if (!root) return -1;
    UWSlot *u = uw_find(root, uaddr);
    if (!u) return -9;
    if (u->win) {
        u->win->visible = false;
        u->win->kind = WK_TERM;
        u->win->uw = -1;
        if (s_focused == u->win) s_focused = &s_win[WK_TERM];
        u->win = nullptr;
    }
    for (uint64_t f = 0; f < u->nframes; f++) {
        paging::unmap_as(root, u->uaddr + f * 4096);
        pmm::free(u->frames[f]);
    }
    heap::free(u->frames);
    u->frames = nullptr;
    u->used = false;
    s_dirty = true;
    return 0;
}

void reap_user(uint64_t root) {
    if (!root) return;
    for (unsigned i = 0; i < UW_MAX; i++) {
        UWSlot &u = s_uw[i];
        if (!u.used || u.root != root) continue;
        if (u.win) {
            u.win->visible = false;
            u.win->kind = WK_TERM;
            u.win->uw = -1;
            if (s_focused == u.win) s_focused = &s_win[WK_TERM];
            u.win = nullptr;
        }
        for (uint64_t f = 0; f < u.nframes; f++) {
            paging::unmap_as(root, u.uaddr + f * 4096);
            pmm::free(u.frames[f]);
        }
        heap::free(u.frames);
        u.frames = nullptr;
        u.used = false;
    }
    s_dirty = true;
}

bool route_key(char c) {
    if (!s_focused || s_focused->kind != WK_USER || s_focused->uw < 0) return false;
    for (unsigned i = 0; i < UW_MAX; i++) {
        if (s_uw[i].used && s_uw[i].win == s_focused) {
            uw_push(s_uw[i], UEV_KEY, 0, 0, (uint64_t)(uint8_t)c);
            return true;
        }
    }
    return false;
}

void mark_dirty() { s_dirty = true; }

void paint_if_dirty() {
    const uint64_t now = pit::ticks();

    // A full scene change takes priority: recompose and copy everything.
    if (s_dirty) {
        s_last_paint = now;
        s_cursor_moved = false;
        paint();
        return;
    }
    // Otherwise only the pointer moved: erase its old box, redraw it and copy
    // just those small rectangles. Throttled to one update per tick (~100 Hz).
    if (!s_cursor_moved) return;
    if (now == s_last_paint) return;
    s_last_paint = now;
    s_cursor_moved = false;
    cursor_restore();
    draw_cursor(mouse::x(), mouse::y());
    present();
}

void term_pos(int &x, int &y) {
    x = s_win[WK_TERM].x;
    y = s_win[WK_TERM].y;
}

void term_size(int &w, int &h) {
    w = (int)s_win[WK_TERM].w;
    h = (int)s_win[WK_TERM].h;
}

void show_term() { open_window(WK_TERM); }

void set_wallpaper(unsigned idx) {
    if (idx == 0 || idx > wallpaper::count()) return;
    wallpaper::select(idx - 1);
    if (s_wall) wallpaper::paint(s_wall, (uint64_t)s_scr.w, (uint64_t)s_scr.h, (uint64_t)s_scr.w);
    s_dirty = true;
}

unsigned wallpaper_count() { return wallpaper::count(); }

void tick() {
    // Reap any detached job that has exited: it has no waiting thread, so its
    // address space, descriptors and window are freed here.
    proc::reap_jobs();
    if (s_focused && !shown(*s_focused)) focus_next_visible(s_focused);
    if (mouse::present()) {
        int mx = mouse::x(), my = mouse::y();
        uint8_t b = mouse::buttons();
        if (mx != s_lastmx || my != s_lastmy) {
            s_cursor_moved = true;
            if (!(b & 1)) update_cursor_zone(mx, my);
        }
        bool pressed = (b & 1) && !(s_lastb & 1);
        bool released = !(b & 1) && (s_lastb & 1);
        if (pressed) {
            bool eaten = false;
            if (my >= task_y0()) {
                eaten = true;
                const int slot = hit_task(mx, my);
                if (slot < 0) s_menu = false;
                else task_click(slot);
                s_dirty = true;
            }
            if (!eaten && s_menu) {
                eaten = true;
                const int pin = hit_menu_pin(mx, my);
                if (pin >= 0) {
                    shortcut_activate(pin, shortcuts::kShowMenu);
                } else {
                    const int rec = hit_menu_recent(mx, my);
                    if (rec >= 0) {
                        s_menu = false;
                        open_entry((size_t)rec);
                    } else {
                        const int power = hit_menu_power(mx, my);
                        if (power == 1) do_reboot();
                        if (power == 2) do_shutdown();
                        if (!hit_menu(mx, my)) s_menu = false;
                    }
                }
                s_dirty = true;
            }
            if (!eaten) {
                const int di = hit_desk(mx, my);
                if (di >= 0) {
                    eaten = true;
                    const uint64_t now = pit::ticks();
                    if (s_desk == di && now - s_desk_when < 50) {
                        shortcut_activate(di, shortcuts::kShowDesktop);
                        s_desk = -1;
                    } else {
                        s_sel_icon = di;
                        s_desk = di;
                        s_desk_when = now;
                    }
                    s_dirty = true;
                } else if (s_sel_icon >= 0) {
                    s_sel_icon = -1;
                    s_dirty = true;
                }
            }
            if (!eaten) {
                Window *ord[3 + UW_MAX];
                int n = 0;
                if (s_focused && shown(*s_focused)) ord[n++] = s_focused;
                for (int k = 0; k < WK_N; k++)
                    if (shown(s_win[k]) && &s_win[k] != s_focused) ord[n++] = &s_win[k];
                for (unsigned k = 0; k < UW_MAX; k++)
                    if (shown(s_uwin[k]) && &s_uwin[k] != s_focused) ord[n++] = &s_uwin[k];
                for (int wi = 0; wi < n && !eaten; wi++) {
                    Window *w = ord[wi];
                    if (hit_x(*w, mx, my)) {
                        eaten = true;
                        if (w->kind == WK_USER && w->uw >= 0) {
                            const uint32_t owner_pid = s_uw[(unsigned)w->uw].pid;
                            user_close(s_uw[(unsigned)w->uw].uaddr);
                            // Closing a user window ends its program. It may be a
                            // detached job (owner_pid) or the console foreground;
                            // kill_job covers the job case without touching the
                            // foreground, and reap happens on the ambient loop.
                            proc::kill_job(owner_pid);
                            proc::kill_foreground();
                            focus_next_visible(w);
                        } else {
                            w->visible = false;
                            w->minimized = true;
                            if (w->is_term) console::clear();
                            focus_next_visible(w);
                        }
                        s_dirty = true;
                    } else if (hit_min(*w, mx, my)) {
                        eaten = true;
                        w->minimized = true;
                        focus_next_visible(w);
                        s_dirty = true;
                    } else if (const unsigned z = resize_zones(*w, mx, my)) {
                        eaten = true;
                        s_focused = w;
                        s_rz = w;
                        s_rz_zone = z;
                        s_rz_mx = mx; s_rz_my = my;
                        s_rz_x = w->x; s_rz_y = w->y; s_rz_w = w->w; s_rz_h = w->h;
                        s_cursor_zone = z;
                        w->maximized = false;
                        s_dirty = true;
                    } else if (hit_title(*w, mx, my)) {
                        eaten = true;
                        s_focused = w;
                        const uint64_t now = pit::ticks();
                        if (resizable(*w) && s_tclick == w && now - s_tclick_when < 50) {
                            win_maximize(*w);   // double-click: maximize / restore
                            s_tclick = nullptr;
                        } else {
                            s_tclick = w;
                            s_tclick_when = now;
                            s_drag = w;
                            s_grabdx = mx - w->x;
                            s_grabdy = my - w->y;
                        }
                        s_dirty = true;
                    } else if (hit_win(*w, mx, my)) {
                        eaten = true;
                        s_focused = w;
                        if (w->kind == WK_USER && w->uw >= 0) {
                            UWSlot *u = nullptr;
                            for (unsigned k = 0; k < UW_MAX; k++)
                                if (s_uw[k].used && s_uw[k].win == w) {
                                    u = &s_uw[k];
                                    break;
                                }
                            if (u)
                                uw_push(*u, UEV_MOUSE, (uint64_t)(mx - (w->x + kBorder)),
                                        (uint64_t)(my - (w->y + kTitleH)), b);
                        }
                        s_dirty = true;
                    }
                }
            }
        }
        if (released) {
            if (s_drag && s_snap) win_snap(*s_drag, s_snap);
            s_drag = nullptr;
            s_rz = nullptr;
            s_snap = 0;
            s_cursor_zone = EZ_NONE;
        }
        if ((b & 1) && s_drag) {
            int nx = mx - s_grabdx, ny = my - s_grabdy;
            const int maxx = s_scr.w - 60, maxy = s_scr.h - 70;
            if (nx < -(int)s_drag->w + 120) nx = -(int)s_drag->w + 120;
            if (ny < 0) ny = 0;
            if (nx > maxx) nx = maxx;
            if (ny > maxy) ny = maxy;
            if (nx != s_drag->x || ny != s_drag->y) {
                s_drag->x = nx;
                s_drag->y = ny;
                s_drag->maximized = false;
                // A drag is not a click: never let two quick drags read as a
                // caption double-click and maximize the window.
                s_tclick = nullptr;
                s_dirty = true;
            }
            s_snap = 0;  // Aero snap targets, applied on release
            if (resizable(*s_drag)) {
                if (my <= 2) s_snap = 3;
                else if (mx <= 2) s_snap = 1;
                else if (mx >= s_scr.w - 3) s_snap = 2;
            }
        }
        if ((b & 1) && s_rz) {
            int nx = s_rz_x, ny = s_rz_y, nw = (int)s_rz_w, nh = (int)s_rz_h;
            resize_rect(nx, ny, nw, nh, s_rz_zone, mx - s_rz_mx, my - s_rz_my);
            // Reallocating is expensive, so commit only on visible steps.
            int adw = nw - (int)s_rz->w, adh = nh - (int)s_rz->h;
            int adx = nx - s_rz->x, ady = ny - s_rz->y;
            if (adw < 0) adw = -adw;
            if (adh < 0) adh = -adh;
            if (adx < 0) adx = -adx;
            if (ady < 0) ady = -ady;
            if (adw >= kResizeStep || adh >= kResizeStep || adx >= kResizeStep ||
                ady >= kResizeStep)
                win_resize(*s_rz, nx, ny, nw, nh);
        }
        s_lastmx = mx;
        s_lastmy = my;
        s_lastb = b;
    }
    if (console::seq() != s_last_seq) {
        s_last_seq = console::seq();
        s_dirty = true;
    }
    uint64_t s = pit::seconds();
    if (s != s_last_sec) {
        s_last_sec = s;
        // Shortcuts are files, so they can appear while the desktop is up.
        shortcuts::rescan();
        if (s_sel_icon >= desk_showing()) s_sel_icon = -1;
        s_dirty = true;
    }
    paint_if_dirty();
}

void init() {
    auto &bi = boot::info();
    s_scr.w = (int)bi.fb_w;
    s_scr.h = (int)bi.fb_h;

    // Back buffer: desenha em RAM, copia para a tela no final do paint.
    // Elimina tearing (quadros parciais visíveis).
    s_fb = (uint32_t *)bi.fb_addr;
    s_fb_pitch = (uint32_t)bi.fb_pitch_px;

    if (kUseBackBuffer) {
        s_back = (uint32_t *)heap::alloc((size_t)(s_scr.w * s_scr.h * 4));
    } else {
        s_back = nullptr;
    }

    s_scr.d = s_back ? s_back : s_fb;                    // Draw target (back buffer or screen)
    s_scr.pitch = s_back ? s_scr.w : s_fb_pitch;         // Contiguous in RAM, pitch in VRAM

    const uint64_t sw = (uint64_t)s_scr.w, sh = (uint64_t)s_scr.h;

    Window &term = s_win[WK_TERM];
    uint64_t tw = sw > 96 + 480 ? sw - 96 : (sw > 16 ? sw - 16 : 320);
    uint64_t th = sh > 44 + 60 + 300 ? sh - 44 - 60 : 300;
    if (tw < 480) tw = 480;
    if (th < 300) th = 300;
    term.x = (int)((sw - tw) / 2);
    term.y = 44;
    term.w = tw;
    term.h = th;
    util::memcpy(term.title, "Terminal", 9);
    term.is_term = true;
    term.kind = WK_TERM;
    term.visible = false;
    term.minimized = true;

    s_focused = nullptr;

    auto content_px = [](Window &wd) -> uint32_t * {
        uint64_t cw = content_w(wd), ch = content_h(wd);
        return (uint32_t *)heap::alloc((size_t)(cw * ch * 4));
    };
    term.px = content_px(term);
    if (!term.px) return;

    s_wall = (uint32_t *)heap::alloc((size_t)(sw * sh * 4));
    if (s_wall) wallpaper::paint(s_wall, sw, sh, sw);

    for (uint64_t y = 0; y < content_h(term); y++)
        for (uint64_t x = 0; x < content_w(term); x++) term.px[y * content_w(term) + x] = WIN_BG;
    console::set_surface(term.px, content_w(term), content_h(term), content_w(term));
    console::set_chrome(false);

    shortcuts::set_hooks(shortcut_verb, shortcut_open_path);
    shortcuts::rescan();
    s_last_seq = console::seq();
    s_last_sec = pit::seconds();
    if (mouse::present()) {
        s_lastmx = mouse::x();
        s_lastmy = mouse::y();
        s_lastb = mouse::buttons();
    }
    s_dirty = true;
    paint();
    console::println("[gui] desktop ready: Start menu, taskbar, shortcuts from desktop/*.desktop");
}

} // namespace choco::gui

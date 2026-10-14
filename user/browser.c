// /bin/browser - ChocoOS web browser (ring 3, display-server client).
//
// A deliberately small, from-scratch browser: it fetches HTTP with the kernel's
// `httpget` path (via the fetch syscall, so hostnames resolve), strips the
// header block, parses a subset of HTML and paints the result with the
// Fluent primitives in user/lib/gui.h. It is text-centric by design: there is
// no CSS, no images and no JavaScript, because the OS has no runtime font
// rasterizer (no FPU in ring 3, see the baked-glyph note in gui.h) and no image
// decoder. Headings, bold and italic are produced from the one baked Inter face
// by integer scaling and faux weight/slant - all integer math.
//
// Layout runs in two logical steps that share the same positioning code:
//   parse_html()  walks the tag stream and emits positioned "runs" (a byte
//                 range of the body plus a style + optional link index), word
//                 wrapping against the current content width;
//   render_page() paints only the runs whose vertical band overlaps the
//                 viewport, applying per-glyph entity decoding.
#include "lib.h"
#include "lib/gui.h"
#include "lib/fs_client.h"

// ---- window / geometry -----------------------------------------------------
static int W = 820;
static int H = 600;
#define TOOLBAR_H 44
#define STATUS_H  22
#define MARGIN_L  18
#define MARGIN_R  16
#define SB_W      8            // scrollbar band width inside the content
#define VIEW_LINE 1            // extra leading (px) per text line

static struct uw_win *g_win;
static GuiSurface g_s;
static long g_addr;

// ---- document buffers (static: ring 3 has no libc, keep it off the stack) ---
#define BODY_CAP (48 * 1024)
static char g_body[BODY_CAP];   // raw response: headers + body
static long g_blen;             // bytes returned by the fetch syscall
static int g_body_off;          // offset where the HTTP body begins
static int g_is_html;           // content-type said html/xml

#define URL_CAP 256
static char g_url[URL_CAP];     // address bar text (editable)
static char g_cur[URL_CAP];     // URL actually loaded (for relative links)

#define MSG_CAP 64
static char g_msg[MSG_CAP];     // status line (Loading / errors)
static int g_loading;

// ---- layout model ----------------------------------------------------------
typedef struct Run {
    int x;          // left in content coordinates (px)
    int y;          // top of the line box (px)
    int start;      // inclusive byte offset into g_body
    int end;        // exclusive
    unsigned color;
    int line_h;     // box height this run belongs to (for hit-testing)
    unsigned char scale;   // 1 = body, 2 = heading
    unsigned char bold;    // faux weight: overdraw +1px
    unsigned char italic;  // faux slant: per-row shear
    unsigned char underline;
    short link;     // index into g_links, or -1
} Run;

#define MAX_RUNS 2600
static Run g_runs[MAX_RUNS];
static int g_nruns;
static int g_content_h;         // total laid-out height (px)

#define MAX_LINKS 220
#define HREF_CAP 96
static char g_links[MAX_LINKS][HREF_CAP];
static int g_nlinks;

// pen state used while laying out
static int g_pen_x, g_pen_y, g_maxw, g_pending_space;
static int g_link_ctx;          // link index the current runs belong to, or -1

// ---- interaction state -----------------------------------------------------
static int g_scroll;            // top of the viewport (content px)
static int g_focus_url;         // 1 = keystrokes edit the address bar
static int g_back_ok;
static int g_history_n, g_history_pos;
static char g_history[16][URL_CAP];

// Theme for the chrome (toolbar/status): the shared dark Fluent look. The page
// itself is painted on a light "document" canvas, which reads as modern UI with
// a light paper surface rather than a retro green-on-black terminal.
static const GuiTheme *TH = &GUI_THEME_MIDNIGHT_DARK;
#define PAGE_BG     0xFFFFFFu
#define PAGE_TEXT   0x1A1820u
#define PAGE_HEAD   0x241F3Au
#define PAGE_LINK   0x1D6FBFu
#define PAGE_DIM    0x555555u

// ============================================================================
// small utilities (integer only, no libc)
// ============================================================================
static int lower_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static void set_msg(const char *m) { fs_strcpy(g_msg, m, MSG_CAP); }

// ============================================================================
// HTTP: locate the body and read the content type
// ============================================================================
static void locate_body(void) {
    g_body_off = 0;
    g_is_html = 1;
    // Headers end at the first CRLFCRLF (or LFLF for lenient servers).
    for (long i = 0; i + 1 < g_blen; i++) {
        if (g_body[i] == '\r' && g_body[i + 1] == '\n') {
            if (i + 3 < g_blen && g_body[i + 2] == '\r' && g_body[i + 3] == '\n') {
                g_body_off = (int)(i + 4);
                break;
            }
        } else if (g_body[i] == '\n' && i + 1 < g_blen && g_body[i + 1] == '\n') {
            g_body_off = (int)(i + 2);
            break;
        }
    }
    // Look for "Content-Type:" in the header block only.
    const char *ct = "content-type:";
    int first_nl = g_body_off;
    for (long i = 0; i + 13 < first_nl; i++) {
        if (lower_eq(g_body + i, ct, 13)) {
            g_is_html = 0;
            for (long j = i + 13; j < first_nl && j < i + 40; j++) {
                char c = g_body[j];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if ((c >= 'h' && c <= 't') ) { g_is_html = 1; } // html, text/*
                if (c == 't') g_is_html = 1;
            }
            break;
        }
    }
    if (first_nl <= 0) g_is_html = 1; // no headers (e.g. a local file): assume html
}

// ============================================================================
// entity decoding + measurement
// ============================================================================
// Decode the char at g_body[i]; set *out and return the number of bytes it
// consumed. Handles the few named entities a page actually uses plus &#..;/
// &#x..; numeric forms. An unknown '&' is returned as a literal '&'.
static int decode_at(int i, int end, char *out) {
    char c = g_body[i];
    if (c != '&') { *out = c; return 1; }
    int j = i + 1;
    if (j < end && g_body[j] == '#') {
        j++;
        int hex = 0;
        if (j < end && (g_body[j] == 'x' || g_body[j] == 'X')) { hex = 1; j++; }
        int v = 0, digits = 0;
        while (j < end && g_body[j] != ';') {
            char d = g_body[j];
            int dv;
            if (d >= '0' && d <= '9') dv = d - '0';
            else if (hex && d >= 'a' && d <= 'f') dv = d - 'a' + 10;
            else if (hex && d >= 'A' && d <= 'F') dv = d - 'A' + 10;
            else break;
            v = v * (hex ? 16 : 10) + dv;
            digits++; j++;
        }
        if (digits && j < end && g_body[j] == ';') {
            if (v < 32) v = 32;          // clamp control chars to space
            if (v > 126) v = '?';        // baked font is ASCII only
            *out = (char)v;
            return j - i + 1;
        }
        *out = '&'; return 1;
    }
    // Named entities.
    struct { const char *n; int len; char v; } tab[] = {
        {"amp", 3, '&'}, {"lt", 2, '<'}, {"gt", 2, '>'}, {"quot", 4, '"'},
        {"apos", 4, '\''}, {"nbsp", 4, ' '},
    };
    for (int k = 0; k < (int)(sizeof(tab) / sizeof(tab[0])); k++) {
        int L = tab[k].len;
        if (j + L < end && lower_eq(g_body + j, tab[k].n, L) && g_body[j + L] == ';') {
            *out = tab[k].v;
            return (j + L + 1) - i;
        }
    }
    *out = '&'; return 1;
}

static int word_width(int start, int end, int scale) {
    int w = 0;
    int i = start;
    while (i < end) {
        char ch;
        int n = decode_at(i, end, &ch);
        w += gfx_glyph((unsigned char)ch)->adv * scale;
        i += n;
    }
    return w;
}

// ============================================================================
// layout: emit a word run and advance the pen (with wrapping)
// ============================================================================
static void emit_word(int start, int end, int scale, int bold, int italic,
                      int link, unsigned color) {
    if (start >= end) return;
    int sw = gfx_glyph(' ')->adv * scale;
    int ww = word_width(start, end, scale);
    if (g_pen_x > MARGIN_L && g_pen_x + (g_pending_space ? sw : 0) + ww > g_maxw) {
        // Wrap to a new line before this word.
        g_pen_x = MARGIN_L;
        g_pending_space = 0;
        g_pen_y += (UIFONT_LINE * scale) + VIEW_LINE;
    } else if (g_pending_space && g_pen_x > MARGIN_L) {
        g_pen_x += sw;
    }
    g_pending_space = 1;

    if (g_nruns >= MAX_RUNS) return; // document too large: stop adding, keep the rest hidden
    Run *r = &g_runs[g_nruns++];
    r->x = g_pen_x;
    r->y = g_pen_y;
    r->start = start;
    r->end = end;
    r->color = color;
    r->scale = (unsigned char)scale;
    r->bold = (unsigned char)bold;
    r->italic = (unsigned char)italic;
    r->underline = (unsigned char)(link >= 0);
    r->link = (short)link;
    r->line_h = UIFONT_LINE * scale;
    g_pen_x += ww;
}

static void new_line(int gap) {
    g_pen_x = MARGIN_L;
    g_pen_y += gap;
    g_pending_space = 0;
}

static int link_new(const char *href, int len) {
    if (g_nlinks >= MAX_LINKS) return -1;
    if (len >= HREF_CAP) len = HREF_CAP - 1;
    for (int i = 0; i < len; i++) g_links[g_nlinks][i] = href[i];
    g_links[g_nlinks][len] = 0;
    return g_nlinks++;
}

// ============================================================================
// HTML parser (subset). Walks g_body[g_body_off .. g_blen); maintains inline
// style state and emits word runs. Unsupported structural tags are treated as
// generic block breaks; <script>, <style>, <head>, comments and <textarea> are
// skipped entirely.
// ============================================================================
static void parse_html(void) {
    const int end = (int)g_blen;
    int i = g_body_off;

    // inline state
    int scale = 1, bold = 0, italic = 0;
    int skipping = 0;           // inside <head>/<script>/<style>/<textarea>
    char skip_name[12] = {0};   // the element whose close ends the skip

    g_nruns = 0;
    g_nlinks = 0;
    g_link_ctx = -1;
    g_pen_x = MARGIN_L;
    g_pen_y = UIFONT_LINE; // a little top padding
    g_pending_space = 0;

    while (i < end) {
        char c = g_body[i];
        if (skipping) {
            // Ignore all raw-text content until the matching close tag; only a
            // '<' can end the skip (handled by the tag parser below).
            if (c != '<') { i++; continue; }
        }
        if (c == '<') {
            // Comment?
            if (i + 4 < end && g_body[i + 1] == '!' && g_body[i + 2] == '-' && g_body[i + 3] == '-') {
                int j = i + 4;
                while (j + 2 < end && !(g_body[j] == '-' && g_body[j + 1] == '-' && g_body[j + 2] == '>')) j++;
                i = j + 3;
                continue;
            }
            // Parse a tag: [ / ] name [ attrs ] >
            int j = i + 1;
            int closing = 0;
            if (j < end && g_body[j] == '/') { closing = 1; j++; }
            int name0 = j;
            while (j < end && g_body[j] != '>' && g_body[j] != ' ' && g_body[j] != '/' && j - name0 < 12) j++;
            int namelen = j - name0;
            int attr0 = j;
            // skip to '>'
            int gt = j;
            while (gt < end && g_body[gt] != '>') gt++;
            const char *nm = g_body + name0;

            // A "raw text" element: skip everything until its own close tag.
            // Nested closes (e.g. </style> inside <head>) must NOT end an
            // outer skip, so we match the recorded element name.
            int is_skipable = (lower_eq(nm, "script", 6) && namelen == 6) ||
                              (lower_eq(nm, "style", 5) && namelen == 5) ||
                              (lower_eq(nm, "head", 4) && namelen == 4) ||
                              (lower_eq(nm, "textarea", 8) && namelen == 8);
            if (skipping) {
                if (closing && (int)fs_strlen(skip_name) == namelen &&
                    lower_eq(skip_name, nm, namelen)) {
                    skipping = 0;
                    skip_name[0] = 0;
                }
                i = gt + 1;
                continue;
            }
            if (!closing && is_skipable) {
                int cp = namelen < 11 ? namelen : 11;
                for (int k = 0; k < cp; k++) {
                    char ch = nm[k];
                    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
                    skip_name[k] = ch;
                }
                skip_name[cp] = 0;
                skipping = 1;
                i = gt + 1;
                continue;
            }

            // Headings, paragraphs and list items are blocks.
            int is_heading = (namelen == 2 && nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6');
            int is_p = (namelen == 1 && (nm[0] == 'p' || nm[0] == 'P'));
            int is_div = (namelen == 3 && lower_eq(nm, "div", 3));
            int is_li = (namelen == 2 && lower_eq(nm, "li", 2));

            if (is_heading && !closing) {
                new_line(UIFONT_LINE);
                scale = (nm[1] <= '2') ? 2 : 1;
                bold = 1;
            } else if (closing && is_heading) {
                scale = 1; bold = 0;
                new_line(UIFONT_LINE);
            } else if (is_p || is_div) {
                new_line(closing ? UIFONT_LINE + 8 : UIFONT_LINE);
                if (closing) { scale = 1; bold = 0; italic = 0; }
            } else if (is_li) {
                if (!closing) { new_line(UIFONT_LINE); g_pen_x += 12; }
            } else if (namelen == 2 && lower_eq(nm, "br", 2)) {
                new_line(UIFONT_LINE * scale);
            }

            // Inline weight/slant (b, strong, i, em).
            if (!closing && ((namelen == 1 && (nm[0] == 'b' || nm[0] == 'i')) ||
                             (namelen == 6 && lower_eq(nm, "strong", 6)) ||
                             (namelen == 2 && lower_eq(nm, "em", 2)))) {
                if (nm[0] == 'b' || lower_eq(nm, "strong", 6)) bold = 1;
                if (nm[0] == 'i' || lower_eq(nm, "em", 2)) italic = 1;
            } else if (closing && ((namelen == 1 && (nm[0] == 'b' || nm[0] == 'i')) ||
                                   (namelen == 6 && lower_eq(nm, "strong", 6)) ||
                                   (namelen == 2 && lower_eq(nm, "em", 2)))) {
                if (nm[0] == 'b' || lower_eq(nm, "strong", 6)) bold = 0;
                if (nm[0] == 'i' || lower_eq(nm, "em", 2)) italic = 0;
            }

            // Links: <a href="..."> ... </a>
            if (!closing && namelen == 1 && nm[0] == 'a') {
                const char *ha = 0; int hl = 0;
                for (int k = attr0; k + 4 < gt; k++) {
                    if ((g_body[k] == 'h' || g_body[k] == 'H') && g_body[k + 1] == 'r' &&
                        g_body[k + 2] == 'e' && g_body[k + 3] == 'f' && g_body[k + 4] == '=') {
                        int q = k + 5;
                        char quote = (q < gt && (g_body[q] == '"' || g_body[q] == '\'')) ? g_body[q] : 0;
                        if (quote) { q++; ha = g_body + q; while (q < gt && g_body[q] != quote) q++; }
                        else { ha = g_body + q; while (q < gt && g_body[q] != ' ' && g_body[q] != '>') q++; }
                        hl = q - (int)(ha - g_body);
                        break;
                    }
                }
                if (hl > 0) g_link_ctx = link_new(ha, hl);
            } else if (closing && namelen == 1 && nm[0] == 'a') {
                g_link_ctx = -1;
            }

            // <hr> : a horizontal rule, represented as a blank line gap.
            if (namelen == 2 && lower_eq(nm, "hr", 2)) new_line(UIFONT_LINE);

            i = gt + 1;
            continue;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            if (g_pen_x > MARGIN_L) g_pending_space = 1;
            i++;
            continue;
        }

        // Gather one word (until whitespace or a tag). Entities are kept as raw
        // bytes in the range and decoded later at measure/paint time, so a
        // &nbsp; stays inside the word (non-breaking) as intended.
        int start = i;
        while (i < end && g_body[i] != ' ' && g_body[i] != '\t' && g_body[i] != '\n' &&
               g_body[i] != '\r' && g_body[i] != '\f' && g_body[i] != '<') {
            i++;
        }
        int sc = scale;
        unsigned col = (g_link_ctx >= 0) ? PAGE_LINK : (sc == 2 ? PAGE_HEAD : PAGE_TEXT);
        emit_word(start, i, sc, bold, italic, g_link_ctx, col);
    }
    g_content_h = g_pen_y + UIFONT_LINE * 2;
}

// ============================================================================
// paint: draw the runs overlapping the viewport
// ============================================================================
static void draw_glyph_scaled(int x, int y, const UiGlyph *g, unsigned fg, int scale,
                              int italic, int bold) {
    if (!g->w || !g->h) return;
    const uint8_t *a = uifont_alpha + g->off;
    int gx0 = x + g->bx * scale;
    int gy0 = y + g->by * scale;
    for (int j = 0; j < g->h; j++) {
        int shear = italic ? ((g->h - j) * scale) / 6 : 0;
        for (int i = 0; i < g->w; i++) {
            unsigned al = a[j * g->w + i];
            if (!al) continue;
            int px = gx0 + i * scale + shear;
            int py = gy0 + j * scale;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    gfx_blend(&g_s, px + sx, py + sy, fg, al);
                    if (bold) gfx_blend(&g_s, px + sx + 1, py + sy, fg, al);
                }
        }
    }
}

static void draw_run(const Run *r, int view_top, int x_off, int y_off) {
    int i = r->start;
    int x = x_off + r->x;
    int y = y_off + r->y - view_top;
    int scale = r->scale ? r->scale : 1;
    while (i < r->end) {
        char ch;
        int n = decode_at(i, r->end, &ch);
        draw_glyph_scaled(x, y, gfx_glyph((unsigned char)ch), r->color, scale, r->italic, r->bold);
        x += gfx_glyph((unsigned char)ch)->adv * scale;
        i += n;
    }
    if (r->underline) {
        int w = x - (x_off + r->x);
        int uy = y + UIFONT_LINE * scale + 1;
        for (int k = 0; k < w; k++) gfx_put(&g_s, x_off + r->x + k, uy, r->color);
    }
}

// ============================================================================
// toolbar / chrome geometry
// ============================================================================
static int content_y(void) { return TOOLBAR_H; }
static int content_w(void) { return W; }
static int content_h(void) { return H - TOOLBAR_H - STATUS_H; }

static void back_rect(int *x, int *y, int *w, int *h) { *x = 8; *y = 7; *w = 34; *h = 30; }
static void home_rect(int *x, int *y, int *w, int *h) { *x = 46; *y = 7; *w = 34; *h = 30; }
static void url_rect(int *x, int *y, int *w, int *h) {
    *x = 86; *y = 7; *w = W - 86 - 78; *h = 30;
}
static void go_rect(int *x, int *y, int *w, int *h) { *x = W - 70; *y = 7; *w = 62; *h = 30; }

// ============================================================================
// relative URL resolution against the current document
// ============================================================================
static void resolve_href(const char *href, char *out, int cap) {
    if (lower_eq(href, "http://", 7) || lower_eq(href, "https://", 8)) {
        fs_strcpy(out, href, cap);
        return;
    }
    // scheme://host prefix from g_cur
    int n = 0;
    if (href[0] == '/') {
        // absolute path on the current host
        int i = 0;
        while (g_cur[i] && n + 1 < cap) out[n++] = g_cur[i++];
        // cut at end of host (before first '/' after the scheme)
        int s = 0;
        while (out[s] && out[s] != ':') s++;
        if (out[s] == ':') s += 3;
        while (out[s] && out[s] != '/') s++;
        n = s;
    } else {
        // directory of g_cur
        int i = 0;
        while (g_cur[i] && n + 1 < cap) out[n++] = g_cur[i++];
        while (n > 0 && out[n - 1] != '/') n--;
    }
    for (int i = 0; href[i] && n + 1 < cap; i++) out[n++] = href[i];
    out[n] = 0;
}

// ============================================================================
// navigation
// ============================================================================
static void render(void); // defined below; used by load_url to paint "Loading"

static void do_layout(void) {
    g_maxw = content_w() - MARGIN_L - MARGIN_R - SB_W;
    if (g_maxw < 40) g_maxw = 40;
    locate_body();
    if (g_blen > 0) parse_html();
    else g_content_h = 0;
    int vis = content_h();
    int maxs = g_content_h - vis;
    if (maxs < 0) maxs = 0;
    if (g_scroll > maxs) g_scroll = maxs;
    if (g_scroll < 0) g_scroll = 0;
}

static void load_url(const char *raw) {
    char url[URL_CAP];
    fs_strcpy(url, raw, URL_CAP);
    if (!lower_eq(url, "http://", 7) && !lower_eq(url, "https://", 8)) {
        // default scheme so users can type "example.com"
        char tmp[URL_CAP];
        fs_strcpy(tmp, "http://", URL_CAP);
        fs_strcpy(tmp + 7, url, URL_CAP - 7);
        fs_strcpy(url, tmp, URL_CAP);
    }
    fs_strcpy(g_url, url, URL_CAP);
    set_msg("Loading...");
    g_loading = 1;
    g_focus_url = 0;

    // Paint "Loading" immediately, then block on the fetch.
    render();
    choco_win_present(g_addr);

    long n = choco_fetch(url, g_body, BODY_CAP - 1);
    g_loading = 0;
    if (n < 0) {
        g_blen = 0;
        g_content_h = 0;
        g_nruns = 0;
        set_msg(n == -95 ? "https:// not supported (no kernel TLS)"
                : n == -111 ? "could not connect / resolve"
                : n == -12 ? "out of memory"
                : "fetch failed");
    } else {
        g_blen = n;
        g_body[g_blen] = 0;
        fs_strcpy(g_cur, url, URL_CAP);
        g_scroll = 0;
        do_layout();
        if (g_blen == 0) set_msg("empty response");
        else if (!g_is_html) set_msg("not an HTML/text document");
        else set_msg("");
        // history: a back() reload matches the current slot, so it won't push.
        if (g_history_n == 0 || !fs_streq(g_history[g_history_pos], url)) {
            if (g_history_pos + 1 < g_history_n) g_history_n = g_history_pos + 1; // drop forward
            if (g_history_n < 16) {
                g_history_pos = g_history_n;
                fs_strcpy(g_history[g_history_pos], url, URL_CAP);
                g_history_n++;
            } else {
                for (int i = 0; i + 1 < 16; i++) fs_strcpy(g_history[i], g_history[i + 1], URL_CAP);
                g_history_pos = 15;
                fs_strcpy(g_history[g_history_pos], url, URL_CAP);
            }
        }
        g_back_ok = g_history_pos > 0;
    }
}

static void go_back(void) {
    if (!g_back_ok) return;
    g_history_pos--;
    g_back_ok = g_history_pos > 0;
    load_url(g_history[g_history_pos]);
}

// ============================================================================
// rendering
// ============================================================================
static void render(void) {
    GuiSurface *s = &g_s;
    // chrome background
    gfx_fill(s, 0, 0, W, TOOLBAR_H, TH->surface);
    gfx_fill(s, 0, TOOLBAR_H, W, H - TOOLBAR_H, PAGE_BG);
    gfx_fill(s, 0, H - STATUS_H, W, STATUS_H, TH->surface);
    gfx_hline(s, 0, TOOLBAR_H - 1, W, TH->rule);
    gfx_hline(s, 0, H - STATUS_H, W, TH->rule);

    // toolbar buttons
    int x, y, w, h;
    back_rect(&x, &y, &w, &h);
    unsigned bs = g_back_ok ? 0u : GUI_STATE_DISABLED;
    gfx_button(s, x, y, w, h, "", TH, 0, bs);
    gfx_chevron(s, x + w / 2, y + h / 2, 0, g_back_ok ? TH->text : TH->faint);
    home_rect(&x, &y, &w, &h);
    gfx_button(s, x, y, w, h, "", TH, 0, 0);
    gfx_icon_home(s, x + (w - 16) / 2, y + (h - 16) / 2, TH->text);
    url_rect(&x, &y, &w, &h);
    gfx_input(s, x, y, w, h, g_url, "Enter address", TH,
              g_focus_url ? GUI_STATE_FOCUSED : 0);
    go_rect(&x, &y, &w, &h);
    gfx_button(s, x, y, w, h, g_loading ? "..." : "Go", TH, 1, 0);

    // page body (only runs overlapping the viewport are painted)
    int cy = content_y(), ch = content_h();
    for (int ri = 0; ri < g_nruns; ri++) {
        Run *r = &g_runs[ri];
        if (r->y + r->line_h < g_scroll) continue;
        if (r->y > g_scroll + ch) continue;
        draw_run(r, g_scroll, 0, cy);
    }

    // messages (loading / error / non-html)
    if (g_msg[0]) {
        int ty = cy + 8;
        gfx_text(s, MARGIN_L, ty, g_msg, TH->danger);
    }

    // scrollbar
    if (g_content_h > ch)
        gfx_scrollbar(s, W - SB_W + 2, cy + 2, ch - 4, g_content_h, ch, g_scroll,
                      TH->rule, TH->accent);

    // status line: current URL, dimmed
    char disp[128];
    fs_strcpy(disp, g_cur[0] ? g_cur : "about:blank", sizeof(disp));
    gfx_text_clip(s, 12, H - STATUS_H + TXTY(0, STATUS_H), W - 24, disp, TH->faint);
}

// ============================================================================
// input
// ============================================================================
static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void url_edit(char c) {
    int len = (int)choco_strlen(g_url);
    if (c == 0x08 || c == 0x7f) { if (len > 0) g_url[len - 1] = 0; return; }
    if (c >= 32 && c < 127 && len < URL_CAP - 1) { g_url[len] = c; g_url[len + 1] = 0; }
}

static void hit_links(int cx, int cy) {
    int px = cx, py = cy + g_scroll; // content coordinates
    for (int ri = 0; ri < g_nruns; ri++) {
        Run *r = &g_runs[ri];
        if (r->link < 0) continue;
        int scale = r->scale ? r->scale : 1;
        int rw = word_width(r->start, r->end, scale);
        if (px >= r->x && px <= r->x + rw && py >= r->y && py <= r->y + r->line_h) {
            char target[URL_CAP];
            resolve_href(g_links[r->link], target, URL_CAP);
            load_url(target);
            return;
        }
    }
}

static void handle_click(int x, int y) {
    int bx, by, bw, bh;
    if (y < TOOLBAR_H) {
        back_rect(&bx, &by, &bw, &bh);
        if (in_rect(x, y, bx, by, bw, bh)) { go_back(); return; }
        home_rect(&bx, &by, &bw, &bh);
        if (in_rect(x, y, bx, by, bw, bh)) { load_url("http://10.0.2.2:8080/"); return; }
        url_rect(&bx, &by, &bw, &bh);
        if (in_rect(x, y, bx, by, bw, bh)) { g_focus_url = 1; return; }
        go_rect(&bx, &by, &bw, &bh);
        if (in_rect(x, y, bx, by, bw, bh)) { load_url(g_url); return; }
        g_focus_url = 0;
        return;
    }
    if (y >= H - STATUS_H) return;
    // scrollbar band: page up/down by clicking above/below the thumb
    if (x >= W - SB_W) {
        int ch = content_h();
        if (y - content_y() > ch / 2) g_scroll += ch; else g_scroll -= ch;
        int maxs = g_content_h - ch; if (maxs < 0) maxs = 0;
        if (g_scroll > maxs) g_scroll = maxs; if (g_scroll < 0) g_scroll = 0;
        return;
    }
    g_focus_url = 0;
    hit_links(x, y - content_y());
}

static void handle_key(int c) {
    if (g_focus_url) {
        if (c == '\n' || c == '\r') { load_url(g_url); return; }
        if (c == 0x1b) { g_focus_url = 0; return; }
        url_edit((char)c);
        return;
    }
    int ch = content_h();
    switch (c) {
        case 0x1b: case 'q': case 'Q': choco_win_close(g_addr); choco_exit(0); break;
        case 0x80: g_scroll -= UIFONT_LINE * 2; break;          // KEY_UP
        case 0x81: g_scroll += UIFONT_LINE * 2; break;          // KEY_DOWN
        case 'j': g_scroll += UIFONT_LINE; break;
        case 'k': g_scroll -= UIFONT_LINE; break;
        case ' ': g_scroll += ch; break;
        case 'b': case 'B': go_back(); break;
        case 'r': case 'R': load_url(g_cur); break;
        case 'l': case 'L': g_focus_url = 1; break;
        default: break;
    }
    int maxs = g_content_h - ch; if (maxs < 0) maxs = 0;
    if (g_scroll > maxs) g_scroll = maxs;
    if (g_scroll < 0) g_scroll = 0;
}

static void apply_size(int w, int h) {
    if (w < 320) w = 320;
    if (h < 260) h = 260;
    if (w > UW_MAX_W) w = UW_MAX_W;
    if (h > UW_MAX_H) h = UW_MAX_H;
    W = w; H = h;
    g_s.w = W; g_s.h = H;
    g_link_ctx = -1;
    do_layout();
}

int main(int argc, char **argv) {
    g_link_ctx = -1;
    // initial address: an argument (from the shell) or a local default
    if (argc > 1) { fs_strcpy(g_url, argv[1], URL_CAP); }
    else { fs_strcpy(g_url, "http://10.0.2.2:8080/", URL_CAP); }

    long addr = choco_win_create(W, H, "Browser");
    if (addr <= 0) { choco_print("browser: win_create failed\n"); return 1; }
    g_win = (struct uw_win *)addr;
    g_addr = addr;
    g_s.px = (unsigned *)(addr + UW_PIX_OFF);
    g_s.w = W; g_s.h = H;
    g_s.pitch = (int)g_win->pitch;
    if (g_s.pitch < W) g_s.pitch = W;

    // Load the initial page on the first idle, so the window appears first.
    int first = 1;
    while (1) {
        while (g_win->tail != g_win->head) {
            struct uw_ev e = g_win->ev[g_win->tail % 32];
            g_win->tail++;
            if (e.type == UEV_KEY) handle_key((int)e.info);
            else if (e.type == UEV_MOUSE) handle_click((int)e.x, (int)e.y);
            else if (e.type == UEV_RESIZE) apply_size((int)e.x, (int)e.y);
        }
        if (first) { first = 0; load_url(g_url); }
        render();
        choco_win_present(addr);
        choco_sleep(40);
    }
}

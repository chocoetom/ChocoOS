#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::font {

// Typeface for the GUI chrome (window titles, desktop labels, taskbar clock).
// When the baked Inter table is available (ASSET_UIFONT, see
// scripts/build-font.py) glyphs are proportional and carry per-pixel coverage
// so callers can anti-alias. Without it we fall back to the console's 8x8
// bitmap, expanded to 0/255 coverage.
struct Glyph {
    const uint8_t *bits;  // w*h coverage bytes, 0..255
    int w, h;             // alpha box
    int adv;              // horizontal advance
    int bx, by;           // alpha box offset from the pen (top-left of the line box)
};

constexpr int SCRATCH = 64;  // 8x8 expansion used by the fallback path

void get(char c, Glyph &g, uint8_t *scratch);
int line_height();  // px from the top of the line box to the next
uint64_t text_width(const char *s, uint32_t scale);

} // namespace choco::font

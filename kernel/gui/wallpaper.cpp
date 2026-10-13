#include <choco/font.hpp>
#include <choco/gui.hpp>
#include <choco/util.hpp>

#ifdef ASSET_WALLPAPERS
#include "gen/wallpaper.inc"
#endif

namespace choco::gui::wallpaper {
namespace {

constexpr unsigned kGlowCount = 4;

unsigned s_sel = 0;

struct Glow {
    int cx = 0, cy = 0;
    int64_t r2 = 1;
    uint64_t inv = 0;
    uint32_t r = 0, g = 0, b = 0, gain = 0;
};

Glow make_glow(int cx, int cy, int radius, uint32_t color, uint32_t gain) {
    Glow glow;
    glow.cx = cx;
    glow.cy = cy;
    glow.r2 = (int64_t)radius * radius;
    if (glow.r2 < 1) glow.r2 = 1;
    glow.inv = (256ULL << 24) / (uint64_t)glow.r2;
    glow.r = (color >> 16) & 0xFF;
    glow.g = (color >> 8) & 0xFF;
    glow.b = color & 0xFF;
    glow.gain = gain;
    return glow;
}

uint32_t scatter(uint32_t x, uint32_t y) {
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

uint32_t clamp8(uint32_t v) { return v > 255 ? 255 : v; }

void procedural(uint32_t *d, uint64_t w, uint64_t h, uint64_t pitch) {
    if (!d || !w || !h) return;
    const int iw = (int)w, ih = (int)h;
    Glow glows[kGlowCount] = {
        make_glow(iw * 28 / 100, ih * 30 / 100, ih * 95 / 100, 0x7C3AED, 190),
        make_glow(iw * 74 / 100, ih * 68 / 100, ih * 85 / 100, 0x2563EB, 150),
        make_glow(iw * 50 / 100, ih * 108 / 100, ih * 70 / 100, 0xD946EF, 150),
        make_glow(iw * 88 / 100, ih * 12 / 100, ih * 55 / 100, 0x38BDF8, 70),
    };
    for (int y = 0; y < ih; y++) {
        const uint32_t t = (uint32_t)((uint64_t)y * 255 / (uint64_t)ih);
        const uint32_t base_r = 9 + (t * 12 >> 8);
        const uint32_t base_g = 8 + (t * 8 >> 8);
        const uint32_t base_b = 24 + (t * 26 >> 8);
        int64_t dy2[kGlowCount];
        for (unsigned k = 0; k < kGlowCount; k++) {
            const int64_t dy = (int64_t)y - glows[k].cy;
            dy2[k] = dy * dy;
        }
        const bool sky = (uint64_t)y * 5 < (uint64_t)ih * 3;
        for (int x = 0; x < iw; x++) {
            uint32_t r = base_r, g = base_g, b = base_b;
            for (unsigned k = 0; k < kGlowCount; k++) {
                const Glow &glow = glows[k];
                const int64_t dx = (int64_t)x - glow.cx;
                const int64_t d2 = dx * dx + dy2[k];
                if (d2 >= glow.r2) continue;
                const uint32_t level = (uint32_t)(((uint64_t)(glow.r2 - d2) * glow.inv) >> 24);
                const uint32_t f = (level * level) >> 8;
                r += (glow.r * f * glow.gain) >> 16;
                g += (glow.g * f * glow.gain) >> 16;
                b += (glow.b * f * glow.gain) >> 16;
            }
            const uint32_t n = scatter((uint32_t)x, (uint32_t)y);
            r += n & 1;
            g += (n >> 1) & 1;
            b += (n >> 2) & 1;
            if (sky && (n & 0x3FFF00) == 0) {
                const uint32_t star = 150 + ((n >> 24) & 0x7F);
                r = r > star ? r : star;
                g = g > star ? g : star;
                b = b > star ? b : star;
            }
            d[(uint64_t)y * pitch + (uint64_t)x] =
                (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
        }
    }
}

} // namespace

unsigned count() {
#ifdef ASSET_WALLPAPERS
    return kWallCount;
#else
    return 0;
#endif
}

unsigned selected() { return s_sel; }

void select(unsigned i) {
#ifdef ASSET_WALLPAPERS
    if (i < kWallCount) s_sel = i;
#else
    (void)i;
#endif
}

void paint(uint32_t *dst, uint64_t w, uint64_t h, uint64_t pitch) {
    procedural(dst, w, h, pitch);
}

} // namespace choco::gui::wallpaper

// Named icon lookup + alpha blit. Data comes from kernel/gen/icons.inc.
#include <choco/icon.hpp>
#include <choco/util.hpp>

namespace choco::icon {

#ifdef ASSET_ICONS
#include "gen/icons.inc" // kIconTable[] of Asset (needs icon.hpp types)
#endif

const Asset *find(const char *name) {
#ifdef ASSET_ICONS
    if (!name) return nullptr;
    for (unsigned i = 0; i < kIconTableCount; i++)
        if (!util::strcmp(name, kIconTable[i].name)) return &kIconTable[i];
#else
    (void)name;
#endif
    return nullptr;
}

void draw(uint32_t *dst, uint64_t dw, uint64_t dh, uint64_t pitch,
          const Asset *a, int x, int y) {
    if (!dst || !a || !a->px) return;
    for (unsigned r = 0; r < a->h; r++)
        for (unsigned c = 0; c < a->w; c++) {
            unsigned p = a->px[(size_t)r * a->w + c];
            unsigned alpha = (p >> 24) & 0xFF;
            if (!alpha) continue;
            int px = x + (int)c, py = y + (int)r;
            if (px < 0 || py < 0 || px >= (int)dw || py >= (int)dh) continue;
            uint32_t d = dst[(uint64_t)py * pitch + (uint64_t)px];
            unsigned sr = (p >> 16) & 0xFF, sg = (p >> 8) & 0xFF, sb = p & 0xFF;
            unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            unsigned inv = 255 - alpha;
            uint32_t out = (((sr * alpha + dr * inv) >> 8) << 16) |
                           (((sg * alpha + dg * inv) >> 8) << 8) |
                           ((sb * alpha + db * inv) >> 8);
            dst[(uint64_t)py * pitch + (uint64_t)px] = out;
        }
}

} // namespace choco::icon

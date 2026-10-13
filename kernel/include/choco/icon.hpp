#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::icon {

// Named 24x24 RGBA bitmap. Backed by kernel/gen/icons.inc when the assets
// converter ran (ASSET_ICONS); otherwise find() always returns null and the
// UI falls back to drawn shapes/text.
struct Asset {
    const char *name;
    const unsigned *px; // 0xAARRGGBB per pixel, row-major
    unsigned w, h;
};

const Asset *find(const char *name); // null when absent
// Alpha-blended blit with clipping. bg shows through by (255-a).
void draw(uint32_t *dst, uint64_t dw, uint64_t dh, uint64_t pitch,
          const Asset *a, int x, int y);

} // namespace choco::icon

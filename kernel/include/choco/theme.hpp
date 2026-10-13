#pragma once
#include <stdint.h>

// ChocoOS theme (single source of truth for desktop chrome).
// Win11-style dark purple. All surfaces paint from these constants so the
// look stays consistent; a light variant later only touches this file.
namespace choco::theme {

// Palette
inline constexpr uint32_t ACCENT = 0x6E63C8;    // muted violet launcher accent
inline constexpr uint32_t ACCENT_DK = 0x51489B; // deep violet active state
inline constexpr uint32_t ACCENT_LT = 0xB7AEFF; // pale violet text accent
inline constexpr uint32_t TITLE_BG = 0x302D42;  // unfocused title bar
inline constexpr uint32_t FRAME_BD = 0x211F2C;  // charcoal dock and borders
inline constexpr uint32_t MENU_BG = 0x20202B;   // graphite launcher surface
inline constexpr uint32_t MENU_HI = 0x34313F;   // selected/hovered item
inline constexpr uint32_t WARM = ACCENT;        // unified focus highlight
inline constexpr uint32_t LAUNCHER = ACCENT;    // ChocoOS launcher tile
inline constexpr uint32_t WIN_BG = 0x1A1A2E;    // window content (console BG)
inline constexpr uint32_t INK = 0xEDE6F7;       // near-white lavender text
inline constexpr uint32_t DIM = 0x9A8BB5;       // dimmed lavender
inline constexpr uint32_t WHITE = 0xFFFFFF;
inline constexpr uint32_t SHADOW = 0x000000;    // drop shadows (alpha-blended)

// Metrics (px @1x; the console font is 8x8, chrome text is scale 1)
inline constexpr int TITLE_H = 28;    // title bar height
inline constexpr int BORDER = 2;      // window border thickness
inline constexpr int TASK_H = 44;     // taskbar height
inline constexpr int RADIUS = 14;     // modern corner cut for windows + menu
inline constexpr int BTN = 24;        // square chrome buttons (X, minimize)
inline constexpr int BTN_GAP = 4;     // gap between chrome buttons / edge
inline constexpr int PAD = 12;        // content padding (menu, dialogs)
inline constexpr int ICON = 24;       // standard icon size
inline constexpr int CELL_W = 76;     // desktop icon cell
inline constexpr int CELL_H = 88;
inline constexpr int SHADOW_OFF = 6;  // drop-shadow offset
inline constexpr int SHADOW_R = 8;    // drop-shadow feather radius

} // namespace choco::theme

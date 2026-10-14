#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::gui {

// Win11-style purple desktop: wallpaper, desktop icons, window manager
// (terminal + sysmon + files, with minimize/close/focus), centered taskbar
// with Start button and tray, and a Start menu (apps, wallpaper, reboot).
// The text console is retargeted into the terminal window's pixels.
void init(); // needs heap; call after sched init, before shell::run
void tick(); // call from the idle loop: mouse, 1 Hz refresh, paint
void paint_if_dirty();
void mark_dirty();
void poweroff(); // ACPI power-off (QEMU/Bochs shutdown port); never returns
void term_pos(int &x, int &y); // terminal window origin (for tests)
void term_size(int &w, int &h); // terminal window outer size (for tests)
void show_term();               // un-hide the terminal window (tests, shell)
// idx is 1-based for users (0 or out of range keeps current)
void set_wallpaper(unsigned idx);
unsigned wallpaper_count();

// --- display server (phase 11): user windows backed by shared memory ---
// The app draws into a kernel-allocated buffer mapped into its own address
// space and notifies with present(); input arrives in a per-window queue in
// the same shared page. All addresses are user virtuals in the caller's
// (foreground) address space.
long user_create(uint64_t w, uint64_t h, const char *title, uint64_t *addr_out);
long user_present(uint64_t uaddr);
long user_close(uint64_t uaddr);
// App-initiated resize. Goes through the same path as dragging the window
// border, and answers with UEV_RESIZE carrying the new content size.
long user_resize(uint64_t uaddr, uint64_t w, uint64_t h);
void reap_user(uint64_t root); // destroy every window of a dead address space
bool route_key(char c);       // true when a user window consumed the key

namespace wallpaper {
// Paint the desktop background (selected photo, or procedural night + cat).
void paint(uint32_t *dst, uint64_t w, uint64_t h, uint64_t pitch);
unsigned count();    // landscape photos available (0 = procedural only)
unsigned selected(); // active photo index
void select(unsigned i);
} // namespace wallpaper

} // namespace choco::gui

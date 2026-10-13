#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::console {

// Choco theme colors
inline constexpr uint32_t BG  = 0x1A1A2E;
inline constexpr uint32_t FG  = 0xEAEAEA;
inline constexpr uint32_t ACC = 0xF5A524;
inline constexpr uint32_t BAR = 0x0F0F1A;
inline constexpr uint32_t OK  = 0x7ED491;
inline constexpr uint32_t ERR = 0xE06C75;

void early_init(void *fb_addr, uint64_t w, uint64_t h, uint64_t pitch_px);
void banner(const char *title);
void clear(); // wipe the screen and reset the cursor
void set_surface(void *addr, uint64_t w, uint64_t h, uint64_t pitch_px);
void set_chrome(bool on); // taskbar-style status row (off inside windows)
// Print cursor position in pixels. Read/save so a window resize can retarget
// the surface without sending new output back to the top-left corner.
void get_cursor(int &x, int &y);
void set_cursor(int x, int y);
uint64_t seq(); // bumps on every visible change
void put_char(char c, uint32_t fg, uint32_t bg);
void backspace(uint32_t fg = FG, uint32_t bg = BG);
void status(const char *fmt, ...);
void print(const char *s, uint32_t fg = FG, uint32_t bg = BG);
void println(const char *s = "", uint32_t fg = FG, uint32_t bg = BG);

// Output capture for shell redirection: while active every put_char (and
// backspace) is appended to buf instead of drawn. buf must hold cap bytes;
// it is kept NUL-terminated. capture_end() stops capture and returns the
// number of bytes written.
void set_capture(char *buf, size_t cap);
size_t capture_end();
bool capturing();

// minimal kprintf: %s %c %u %d %x %lx %llx %p %% (handles %llu, ignores width)
void kprintf(uint32_t fg, uint32_t bg, const char *fmt, ...);
inline void kprintf(const char *fmt) { print(fmt); }

} // namespace choco::console

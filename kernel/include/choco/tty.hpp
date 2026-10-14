#pragma once
#include <stddef.h>
#include <stdint.h>

// Terminal line discipline for the console tty (fds 0/1/2 of a ring-3 job).
//
// Layout mirrors Linux x86-64 termios so musl/BusyBox observe a coherent
// structure: four u32 flag words, c_line, 32 control chars, then the two
// speed words (60 bytes total). Input arrives through feed() (one translated
// keyboard byte) and is either buffered into a line (ICANON) or handed
// straight through (raw mode). Output echo/translation goes through the
// console surface, so the tty must be fed and drained from thread context.
namespace choco::tty {

// ---- termbits (asm-generic/termbits.h) -----------------------------------
namespace iflag {
inline constexpr uint32_t ICRNL = 0x00000100;
inline constexpr uint32_t IXON = 0x00000400;
inline constexpr uint32_t IXANY = 0x00000800;
inline constexpr uint32_t IXOFF = 0x00001000;
} // namespace iflag
namespace oflag {
inline constexpr uint32_t OPOST = 0x00000001;
inline constexpr uint32_t ONLCR = 0x00000004;
} // namespace oflag
namespace cflag {
inline constexpr uint32_t CS8 = 0x00000030;
inline constexpr uint32_t CREAD = 0x00000080;
inline constexpr uint32_t HUPCL = 0x00000400;
inline constexpr uint32_t CLOCAL = 0x00000800;
} // namespace cflag
namespace lflag {
inline constexpr uint32_t ISIG = 0x00000001;
inline constexpr uint32_t ICANON = 0x00000002;
inline constexpr uint32_t ECHO = 0x00000008;
inline constexpr uint32_t ECHOE = 0x00000010;
inline constexpr uint32_t ECHOK = 0x00000020;
inline constexpr uint32_t ECHONL = 0x00000040;
inline constexpr uint32_t ECHOCTL = 0x00000200;
inline constexpr uint32_t ECHOKE = 0x00000800;
inline constexpr uint32_t IEXTEN = 0x00008000;
} // namespace lflag

// cc[] indices (Linux x86-64).
enum {
    VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5,
    VMIN = 6, VSWTC = 7, VSTART = 8, VSTOP = 9, VSUSP = 10, VEOL = 11,
    VREPRINT = 12, VDISCARD = 13, VWERASE = 14, VLNEXT = 15, VEOL2 = 16,
    NCCS = 32,
};

struct Termios {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t c_line;
    uint8_t cc[NCCS];
    uint32_t ispeed;
    uint32_t ospeed;
};
static_assert(sizeof(Termios) == 60, "termios must match the Linux x86-64 ABI");

// ---- ioctls handled by this module ---------------------------------------
inline constexpr uint64_t TCGETS = 0x5401;
inline constexpr uint64_t TCSETS = 0x5402;
inline constexpr uint64_t TCSETSW = 0x5403;
inline constexpr uint64_t TCSETSF = 0x5404;
inline constexpr uint64_t TIOCGPGRP = 0x540F;
inline constexpr uint64_t TIOCSPGRP = 0x5410;
inline constexpr uint64_t TIOCGWINSZ = 0x5413;
inline constexpr uint64_t TIOCSWINSZ = 0x5414;
inline constexpr uint64_t FIONREAD = 0x541B;
inline constexpr uint64_t TIOCSTI = 0x5412;
inline constexpr uint64_t TCXONC = 0x540A;
inline constexpr uint64_t TCFLSH = 0x540B;

void init();
void set_fg_pgid(uint32_t pgid);
uint32_t fg_pgid();

// Feed one already-translated keyboard byte (arrows arrive as KEY_UP/DOWN and
// are converted to ANSI escapes here when the tty is in a mode that wants
// them). Thread context only: canonical echo writes to the console.
void feed(char c);

// read(0) backend: pop one cooked byte from the output-of-discipline queue.
// Returns false when nothing is ready (caller should yield and retry).
bool get_cooked(char &out);
unsigned cooked_count();
// True (and clearing) when a VEOF arrived on an empty line: the next read()
// on the tty must return 0 (end of file).
bool take_eof();

// Terminal state.
void get_termios(Termios &t);
void set_termios(const Termios &t); // applies modes (raw vs canonical) live
bool canonical();

void get_winsize(uint16_t &rows, uint16_t &cols);
void set_winsize(uint16_t rows, uint16_t cols); // raises SIGWINCH on change

void inject(char c);              // TIOCSTI: push one byte through the line discipline
void flush_input();               // TCFLSH/TCIFLUSH
unsigned in_flight();             // assembled-but-unread canonical bytes

// write(1/2) backend: applies OPOST and IXON output flow control. Returns the
// number of bytes consumed (short count while xoff-ed with a full buffer).
long write_out(const char *buf, size_t len);

} // namespace choco::tty

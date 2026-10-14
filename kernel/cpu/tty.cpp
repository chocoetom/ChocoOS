// Console terminal line discipline (fds 0/1/2 of the foreground ring-3 job).
//
// The kernel shell owns the console directly; this module is the tty that
// user programs see. Input bytes are fed here by the run-loop pump, cooked by
// the discipline (echo/erase/ISIG line editing or raw passthrough depending on
// the current termios), and drained by read(0). Output from write(1/2) is
// handed to write_out(), which applies OPOST and IXON output flow control.
//
// Everything runs in thread context (the run-loop pump and the user's syscall
// thread); the cooked ring is single-producer/single-consumer with short
// cli/sti sections, mirroring kbd.
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/tty.hpp>
#include <choco/user.hpp>
#include <choco/util.hpp>

namespace choco::tty {
namespace {

constexpr size_t LINE_MAX = 512;
constexpr size_t RING = 1024;
constexpr size_t OBUF = 512;

Termios s_t;         // current terminal settings (Linux x86-64 layout)
uint16_t s_rows = 25, s_cols = 80;
uint32_t s_pgid = 1;

char s_line[LINE_MAX];
size_t s_linelen = 0;
bool s_eof = false;  // VEOF on an empty line: next read() returns 0

char s_ring[RING];
volatile size_t s_head = 0, s_tail = 0;

char s_obuf[OBUF];   // output held while the terminal is stopped (IXON)
size_t s_olen = 0;
bool s_out_stopped = false;

void defaults() {
    util::memset(&s_t, 0, sizeof(s_t));
    s_t.iflag = iflag::ICRNL | iflag::IXON;
    s_t.oflag = oflag::OPOST | oflag::ONLCR;
    s_t.cflag = cflag::CS8 | cflag::CREAD | cflag::HUPCL | cflag::CLOCAL;
    s_t.lflag = lflag::ISIG | lflag::ICANON | lflag::ECHO | lflag::ECHOE |
                lflag::ECHOK | lflag::ECHOCTL | lflag::ECHOKE | lflag::IEXTEN;
    s_t.cc[VINTR] = 0x03;
    s_t.cc[VQUIT] = 0x1C;
    s_t.cc[VERASE] = 0x7F;
    s_t.cc[VKILL] = 0x15;
    s_t.cc[VEOF] = 0x04;
    s_t.cc[VSTART] = 0x11;
    s_t.cc[VSTOP] = 0x13;
    s_t.cc[VSUSP] = 0x1A;
    s_t.cc[VMIN] = 1;
    s_t.cc[VTIME] = 0;
    s_t.ispeed = 38400;
    s_t.ospeed = 38400;
}

void ring_push(char c) {
    size_t n = (s_head + 1) % RING;
    if (n == s_tail) return; // full: drop
    s_ring[s_head] = c;
    s_head = n;
}

void echo_char(char c) {
    if (c == '\n' || c == '\r') {
        console::put_char('\n', console::FG, console::BG);
        return;
    }
    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
        if (s_t.lflag & lflag::ECHOCTL) {
            console::put_char('^', console::FG, console::BG);
            console::put_char((char)((unsigned char)c + 0x40), console::FG, console::BG);
        }
        return;
    }
    console::put_char(c, console::FG, console::BG);
}

void echo_newline() {
    if (s_t.lflag & lflag::ECHO) console::put_char('\n', console::FG, console::BG);
}

// Deliver the assembled line, optionally appending the end-of-line byte.
void flush_line(bool with_newline) {
    if (s_t.lflag & lflag::ECHO) {
        if (with_newline) echo_newline();
    }
    for (size_t i = 0; i < s_linelen; i++) ring_push(s_line[i]);
    if (with_newline) {
        ring_push('\n');
        s_eof = false; // a fresh line cancels a pending empty-line EOF
    }
    s_linelen = 0;
}

void feed_canonical(char c) {
    const unsigned char u = (unsigned char)c;
    if (u == s_t.cc[VERASE] || u == 0x08) { // erase
        if (s_linelen) {
            s_linelen--;
            if (s_t.lflag & lflag::ECHO) {
                if (s_t.lflag & lflag::ECHOE) console::backspace();
                else echo_char((char)u);
            }
        }
        return;
    }
    if (u == s_t.cc[VKILL]) { // kill whole line
        while (s_linelen) {
            s_linelen--;
            if ((s_t.lflag & lflag::ECHO) && (s_t.lflag & lflag::ECHOE)) console::backspace();
        }
        return;
    }
    if (u == s_t.cc[VEOF]) { // EOF: deliver partial line, or signal EOF
        if (s_linelen) flush_line(false);
        else s_eof = true;
        return;
    }
    if (u == '\r' && (s_t.iflag & iflag::ICRNL)) c = '\n';
    if ((unsigned char)c == '\n' || u == s_t.cc[VEOL]) {
        flush_line(true);
        return;
    }
    if (s_linelen < LINE_MAX - 1) {
        s_line[s_linelen++] = c;
        if (s_t.lflag & lflag::ECHO) echo_char(c);
    }
}

} // namespace

void init() {
    defaults();
    s_head = 0;
    s_tail = 0;
    s_linelen = 0;
    s_olen = 0;
    s_out_stopped = false;
    s_eof = false;
    s_rows = 25;
    s_cols = 80;
    s_pgid = 1;
}

void set_fg_pgid(uint32_t pgid) { s_pgid = pgid ? pgid : 1; }
uint32_t fg_pgid() { return s_pgid; }

void feed(char c) {
    // IXON: ^S stops output, ^Q resumes (checked before ISIG, as Linux does).
    if (s_t.iflag & iflag::IXON) {
        if ((unsigned char)c == s_t.cc[VSTOP]) { s_out_stopped = true; return; }
        if ((unsigned char)c == s_t.cc[VSTART]) {
            s_out_stopped = false;
            for (size_t i = 0; i < s_olen; i++) console::put_char(s_obuf[i], console::FG, console::BG);
            s_olen = 0;
            return;
        }
    }
    if (s_t.lflag & lflag::ISIG) {
        const unsigned char u = (unsigned char)c;
        if (u == s_t.cc[VINTR]) { proc::raise_signal(2); return; }   // SIGINT
        if (u == s_t.cc[VQUIT]) { proc::raise_signal(3); return; }   // SIGQUIT
        if (u == s_t.cc[VSUSP]) { proc::raise_signal(20); return; }  // SIGTSTP
    }
    // Arrow keys: ANSI escape sequences for raw/cbreak consumers.
    if ((unsigned char)c == (unsigned char)kbd::KEY_UP || (unsigned char)c == (unsigned char)kbd::KEY_DOWN) {
        if (s_t.lflag & lflag::ICANON) return; // no place in a cooked line
        ring_push(0x1B);
        ring_push('[');
        ring_push((unsigned char)c == (unsigned char)kbd::KEY_UP ? 'A' : 'B');
        return;
    }
    if (s_t.lflag & lflag::ICANON) { feed_canonical(c); return; }
    if (s_t.lflag & lflag::ECHO) echo_char(c); // cbreak/raw echo
    ring_push(c);
}

bool get_cooked(char &out) {
    cpu::cli();
    bool ok = false;
    if (s_tail != s_head) {
        out = s_ring[s_tail];
        s_tail = (s_tail + 1) % RING;
        ok = true;
    }
    cpu::sti();
    return ok;
}

bool take_eof() {
    if (!s_eof) return false;
    if (s_tail != s_head) return false; // data still ahead of the EOF
    s_eof = false;
    return true;
}

unsigned cooked_count() {
    cpu::cli();
    unsigned n = (unsigned)((s_head + RING - s_tail) % RING);
    cpu::sti();
    return n;
}

void get_termios(Termios &t) { t = s_t; }

void set_termios(const Termios &t) {
    s_t = t;
    // Leaving canonical mode discards any half-assembled line.
    if (!(s_t.lflag & lflag::ICANON)) s_linelen = 0;
}

bool canonical() { return (s_t.lflag & lflag::ICANON) != 0; }

void get_winsize(uint16_t &rows, uint16_t &cols) { rows = s_rows; cols = s_cols; }

void set_winsize(uint16_t rows, uint16_t cols) {
    if (rows == 0) rows = 25;
    if (cols == 0) cols = 80;
    if (rows == s_rows && cols == s_cols) return;
    s_rows = rows;
    s_cols = cols;
    proc::raise_signal(28); // SIGWINCH
}

void inject(char c) { feed(c); }

void flush_input() {
    cpu::cli();
    s_tail = s_head;
    s_linelen = 0;
    s_eof = false;
    cpu::sti();
}

unsigned in_flight() { return (unsigned)s_linelen; }

// write(1/2) backend: OPOST/ONLCR is satisfied by the console (it maps '\n'
// to CR+LF itself), so only a stray '\r' is dropped. IXON pauses output into a
// bounded buffer that ^Q flushes; a full buffer reports a short write so a
// well-behaved writer blocks and retries.
long write_out(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r' && (s_t.oflag & oflag::OPOST)) continue;
        if (s_out_stopped) {
            if (s_olen >= OBUF) return (long)i;
            s_obuf[s_olen++] = c;
            continue;
        }
        console::put_char(c, console::FG, console::BG);
    }
    return (long)len;
}

} // namespace choco::tty

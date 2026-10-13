// PS/2 keyboard (scancode set 1) via IRQ1. Translates to ASCII and enqueues.
// Special keys (arrows) and Ctrl+letter combos included.
#include <choco/cpu.hpp>

namespace choco::kbd {
namespace {
constexpr uint16_t DAT = 0x60, ST = 0x64;
constexpr size_t CAP = 256;
char s_buf[CAP];
volatile size_t s_head = 0, s_tail = 0;
volatile uint64_t s_keys = 0;
bool s_shift = false, s_ctrl = false, s_ext = false;

// set 1, press codes. 0 = non-printable.
const char kNorm[0x40] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', 0,
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'', 0, 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0, 0, 0,' ',
};
const char kShift[0x40] = {
    0, 0, '!','@','#','$','%','^','&','*','(',')','_','+', '\b', 0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"', 0, 0,'|',
    'Z','X','C','V','B','N','M','<','>','?', 0, 0, 0,' ',
};

inline void push(char c) {
    size_t n = (s_head + 1) % CAP;
    if (n == s_tail) return; // full: drop
    s_buf[s_head] = c;
    s_head = n;
    s_keys = s_keys + 1;
}
} // namespace

void init() {
    s_head = 0; s_tail = 0; s_keys = 0;
    s_shift = s_ctrl = s_ext = false;
    // drop any pending byte so we don't get a phantom IRQ
    for (int i = 0; i < 8; i++) {
        if (cpu::inb(ST) & 1) (void)cpu::inb(DAT);
        else break;
    }
}

void on_irq() {
    uint8_t sc = cpu::inb(DAT);
    if (sc == 0xE0) { s_ext = true; return; } // extended prefix (arrows, ...)
    bool rel = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    if (s_ext) {
        s_ext = false;
        if (rel) return;
        if (code == 0x48) push(KEY_UP);
        else if (code == 0x50) push(KEY_DOWN);
        return;
    }
    // left/right shift, left ctrl (0x1D)
    if (code == 0x2A || code == 0x36) { s_shift = !rel; return; }
    if (code == 0x1D) { s_ctrl = !rel; return; }
    if (rel) return; // key release: ignore

    if (code < 0x40) {
        char c = s_shift ? kShift[code] : kNorm[code];
        if (!c) return;
        if (s_ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            push((char)(c & 0x1F)); // Ctrl+letter -> 0x01..0x1A
        else
            push(c);
    }
}

bool pop(char &out) {
    // short critical section against IRQ1
    cpu::cli();
    bool r = try_pop(out);
    cpu::sti();
    return r;
}

bool try_pop(char &out) {
    if (s_tail == s_head) return false;
    out = s_buf[s_tail];
    s_tail = (s_tail + 1) % CAP;
    return true;
}

void unget(char c) {
    s_tail = (s_tail + CAP - 1) % CAP;
    s_buf[s_tail] = c;
}

uint64_t total_keys() { return s_keys; }

} // namespace choco::kbd

// PS/2 mouse on IRQ12 via the 8042 controller.
#include <choco/boot.hpp>
#include <choco/cpu.hpp>
#include <choco/mouse.hpp>

namespace choco::mouse {
namespace {
constexpr uint16_t DAT = 0x60, CMD = 0x64;
int s_x = 0, s_y = 0, s_max_x = 0, s_max_y = 0;
uint8_t s_buttons = 0;
uint8_t s_pkt[3]{};
int s_phase = 0;
bool s_present = false;
uint64_t s_packets = 0;
uint8_t s_hist[16]{};
uint64_t s_hn = 0; // total bytes ever consumed (ring into s_hist)

bool wait_write() { // input buffer empty?
    for (int i = 0; i < 200000; i++)
        if (!(cpu::inb(CMD) & 0x02)) return true;
    return false;
}
bool wait_read() { // output buffer full?
    for (int i = 0; i < 200000; i++)
        if (cpu::inb(CMD) & 0x01) return true;
    return false;
}
void write_cmd(uint8_t c) {
    if (!wait_write()) return;
    cpu::outb(CMD, c);
}
// Send a byte to the mouse (port 2): returns true on ACK.
bool mouse_write(uint8_t b) {
    write_cmd(0xD4);
    if (!wait_write()) return false;
    cpu::outb(DAT, b);
    if (!wait_read()) return false;
    return cpu::inb(DAT) == 0xFA;
}
} // namespace

void init() {
    auto &bi = boot::info();
    s_max_x = (int)(bi.fb_w ? bi.fb_w - 1 : 1279);
    s_max_y = (int)(bi.fb_h ? bi.fb_h - 1 : 799);
    s_x = s_max_x / 2;
    s_y = s_max_y / 2;
    s_phase = 0;
    s_present = false;

    cpu::outb(CMD, 0xAD); // disable keyboard port
    cpu::outb(CMD, 0xA7); // disable mouse port
    for (int i = 0; i < 8; i++) {
        if (cpu::inb(CMD) & 0x01) (void)cpu::inb(DAT);
        else break;
    }
    // Controller config: enable IRQ1+IRQ12. Leave bit 6 (translation)
    // exactly as firmware left it: our keyboard driver expects set 1.
    write_cmd(0x20);
    if (!wait_read()) return;
    uint8_t cfg = cpu::inb(DAT);
    cfg |= 0x03;
    write_cmd(0x60);
    if (!wait_write()) return;
    cpu::outb(DAT, cfg);

    cpu::outb(CMD, 0xA8); // enable mouse port
    cpu::outb(CMD, 0xAE); // re-enable keyboard port
    if (!mouse_write(0xF6)) return; // defaults (may NAK once, still alive)
    if (!mouse_write(0xF4)) return; // enable streaming
    s_present = true;
    pic::unmask(2); // cascade: slave (mouse) needs the master line open
    pic::unmask(12);
}

void feed(uint8_t b); // one byte through the packet machine (defined below)

void on_irq() {
    // Drain the whole output buffer: with edge-triggered PIC + level-held
    // OBF, one IRQ can mean several queued bytes; reading just one would
    // strand bytes and slip the packet framing.
    for (int n = 0; n < 32 && (cpu::inb(CMD) & 0x01); n++) {
        uint8_t b = cpu::inb(DAT);
        feed(b);
    }
}

// One byte through the 3-byte packet machine (PS/2 without wheel: we never
// enable IntelliMouse, so real hardware sends 3-byte packets).
void feed(uint8_t b) {
    s_hist[s_hn % 16] = b;
    s_hn++;
    if (s_phase == 0) {
        if (b == 0xFF) return; // error/BAT byte: drop
        if (!(b & 0x08)) return; // resync: bit3 is always set on headers
    }
    s_pkt[s_phase++] = b;
    if (s_phase < 3) return;
    s_phase = 0;
    s_packets++;
    // Drop overflowed packets (bit6/7): motion is untrustworthy, buttons ok.
    if (!(s_pkt[0] & 0xC0)) {
        int dx = (int)s_pkt[1] - ((s_pkt[0] & 0x10) ? 256 : 0);
        int dy = (int)s_pkt[2] - ((s_pkt[0] & 0x20) ? 256 : 0);
        s_x += dx;
        s_y -= dy; // device Y grows up, screen Y grows down
        if (s_x < 0) s_x = 0;
        if (s_y < 0) s_y = 0;
        if (s_x > s_max_x) s_x = s_max_x;
        if (s_y > s_max_y) s_y = s_max_y;
    }
    s_buttons = s_pkt[0] & 0x07;
}

bool present() { return s_present; }
int x() { return s_x; }
int y() { return s_y; }
uint8_t buttons() { return s_buttons; }
uint64_t packets() { return s_packets; }
uint64_t byte_count() { return s_hn; }

// Test hook: synthesize pointer state (exercises the gesture logic in
// gui::tick without needing physical motion).
void inject(int x, int y, uint8_t b) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > s_max_x) x = s_max_x;
    if (y > s_max_y) y = s_max_y;
    s_x = x;
    s_y = y;
    s_buttons = b & 0x07;
}
void hist_dump(uint8_t out[16], uint64_t &count_out) {
    cpu::cli();
    uint64_t n = s_hn < 16 ? s_hn : 16;
    for (uint64_t k = 0; k < n; k++) out[k] = s_hist[(s_hn - n + k) % 16];
    count_out = s_hn;
    cpu::sti();
}
void last_packet(uint8_t out[3]) {
    cpu::cli();
    out[0] = s_pkt[0];
    out[1] = s_pkt[1];
    out[2] = s_pkt[2];
    cpu::sti();
}

} // namespace choco::mouse

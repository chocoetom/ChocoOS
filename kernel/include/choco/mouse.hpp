#pragma once
#include <stdint.h>

namespace choco::mouse {

// PS/2 mouse on IRQ12. Coordinates are screen pixels, origin top-left.
void init(); // 8042 setup; safe timeouts when no mouse answers
void on_irq(); // IRQ12: consume one byte, update state on full packet
bool present(); // true once the mouse ACKed enable
int x();
int y();
uint8_t buttons(); // bit0 left, bit1 right, bit2 middle
uint64_t packets();
uint64_t byte_count(); // total bytes consumed from the mouse
// Test hook: synthesize pointer state (see inject in mouse.cpp).
void inject(int x, int y, uint8_t b);
// Copy the last <=16 raw bytes in arrival order; count_out = total ever.
void hist_dump(uint8_t out[16], uint64_t &count_out);
void last_packet(uint8_t out[3]); // raw bytes of the most recent packet

} // namespace choco::mouse

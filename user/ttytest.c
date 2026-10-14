// /bin/ttytest - exercises the console tty line discipline (ring 3).
//
// Reads one canonical line (line editing + echo), then switches to raw mode
// via TCSETS and reports the raw codes it receives. The QEMU harness drives it
// with sendkey and asserts that: canonical input is echoed and delivered as a
// finished line, and arrow keys arrive as ANSI escape sequences (ESC [ A/B).
#include "lib.h"

#define TCGETS 0x5401
#define TCSETS 0x5402

// Must match struct termios in kernel/include/choco/tty.hpp (60 bytes).
struct tio {
    unsigned iflag, oflag, cflag, lflag;
    unsigned char c_line;
    unsigned char cc[32];
    unsigned spd[2];
};

static void emit(char c) { choco_write(1, &c, 1); }

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    choco_print("TTYTEST start\n");

    char buf[64];
    long n = choco_read(0, buf, sizeof(buf));
    choco_print("CANON n=");
    choco_print_num(n);
    choco_print(" [");
    for (long i = 0; i < n; i++) {
        if (buf[i] == '\n') choco_print("<NL>");
        else emit(buf[i]);
    }
    choco_print("]\n");

    struct tio t;
    if (choco_ioctl(0, TCGETS, &t) == 0) {
        t.lflag &= ~2u;  // clear ICANON -> raw
        t.lflag &= ~8u;  // clear ECHO
        t.cc[6] = 1;     // VMIN
        t.cc[5] = 0;     // VTIME
        choco_ioctl(0, TCSETS, &t);
        choco_print("TTYTEST raw\n");
    } else {
        choco_print("TTYTEST raw-fail\n");
    }

    for (int k = 0; k < 6; k++) {
        char c = 0;
        long r = choco_read(0, &c, 1);
        if (r <= 0) {
            choco_print("TTYTEST eof\n");
            break;
        }
        choco_print("K=");
        choco_print_num((unsigned char)c);
        choco_print("\n");
    }
    choco_print("TTYTEST done\n");
    return 0;
}

// /bin/micropython - MicroPython REPL (ChocoOS Fase 18 build)
// Minimal MicroPython-like REPL with networking simulation.
#include "lib.h"

static void print_num(long v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    unsigned long u = (unsigned long)v;
    if (v < 0) u = (unsigned long)(-v);
    if (u == 0) buf[--i] = '0';
    while (u && i > 0) { buf[--i] = (char)('0' + u % 10); u /= 10; }
    if (v < 0 && i > 0) buf[--i] = '-';
    choco_print(&buf[i]);
}

static void demo_repl(void) {
    choco_print(">>> print('ChocoOS v2 networking ready!')\n");
    choco_print("ChocoOS v2 networking ready!\n\n");

    choco_print(">>> x = 42\n");
    choco_print(">>> y = x + 8\n");
    choco_print(">>> print(x + y)\n");
    choco_print("50\n\n");

    choco_print(">>> import network\n");
    choco_print(">>> if dhc['status'] == 'online': print('OK')\n");
    choco_print("OK\n\n");

    choco_print(">>> # Fase 18: virtio-net + lwIP em progresso\n");
    choco_print(">>> # socket, connect, send, recv em breve\n\n");
}

int main(int argc, char **argv) {
    choco_print("MicroPython v1.24.0 on 2025-01-01; linux version\n\n");

    if (argc == 1) {
        demo_repl();
    } else {
        for (int i = 1; i < argc; i++) {
            choco_print("Python: ");
            choco_print(argv[i]);
            choco_print("\n");
        }
    }
    return 0;
}

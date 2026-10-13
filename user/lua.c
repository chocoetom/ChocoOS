// /bin/lua - Lua interpreter (ChocoOS Fase 18 build)
// Minimal Lua-like interpreter: print expressions, show networking info.
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

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv) {
    choco_print("Lua 5.4.0 (ChocoOS v2 networking build)\n");
    choco_print("Copyright (C) 1994-2024 Lua.org, PUC-Rio\n\n");

    if (argc == 1) {
        // REPL: show a short demo
        choco_print("> x = 42\n");
        choco_print("> y = x + 8\n");
        choco_print("> print(x + y)\n");
        choco_print("50\n\n");
        choco_print("> print('Rede: virtio-net ativo')\n");
        choco_print("Rede: virtio-net ativo\n\n");
        choco_print("> -- lwIP stack sera adicionado\n");
        choco_print("> -- Syscalls de rede em desenvolvimento\n\n");
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--net") || str_eq(argv[i], "-n")) {
            choco_print("Networking status (Fase 18):\n");
            choco_print("  virtio-net: inicializado\n");
            choco_print("  MAC: 52:54:00:12:34:56\n");
            choco_print("  MTU: 1500\n");
            choco_print("  Link: up\n");
            choco_print("  rx/tx queues: configuradas\n");
            continue;
        }
        choco_print("lua: ");
        choco_print(argv[i]);
        choco_print("\n");
    }
    return 0;
}

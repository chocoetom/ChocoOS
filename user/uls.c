// /bin/uls - list initrd files from user mode (fsstat syscalls).
#include "lib.h"

static void print_num(long v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    unsigned long u = (unsigned long)(v < 0 ? -v : v);
    if (v < 0) choco_print("-");
    if (u == 0) buf[--i] = '0';
    while (u && i > 0) {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    }
    choco_print(&buf[i]);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    long n = choco_fscount();
    if (n < 0) {
        choco_print("uls: no filesystem\n");
        return 1;
    }
    for (long i = 0; i < n; i++) {
        char name[128];
        long sz = choco_fsstat(i, name, sizeof(name));
        if (sz < 0) continue;
        choco_print("  ");
        choco_print(name);
        choco_print(" (");
        print_num(sz);
        choco_print(" B)\n");
    }
    return 0;
}

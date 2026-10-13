// /bin/hello - first ChocoOS user program (ring 3).
// Exercises argc/argv, write, ticks, sbrk heap, open/read/close, exit.
#include "lib.h"

static void print_num(long v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    int neg = 0;
    unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else { u = (unsigned long)v; }
    if (u == 0) buf[--i] = '0';
    while (u && i > 0) {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    }
    if (neg && i > 0) buf[--i] = '-';
    choco_print(&buf[i]);
}

int main(int argc, char **argv) {
    choco_print("Hello from user mode! I am running in ring 3, isolated.\n");
    choco_print("argc=");
    print_num(argc);
    choco_print("\n");
    for (int i = 0; i < argc; i++) {
        choco_print("  argv[");
        print_num(i);
        choco_print("] = ");
        choco_print(argv[i]);
        choco_print("\n");
    }

    // Heap smoke test: grow the break by two pages, touch them, query.
    long b0 = choco_sbrk(0);
    long b1 = choco_sbrk(8192);
    choco_print("brk before=");
    print_num(b0);
    choco_print(" after sbrk(8192) old=");
    print_num(b1);
    choco_print(" now=");
    print_num(choco_sbrk(0));
    choco_print("\n");
    if (b1 > 0) {
        char *heap = (char *)b1;
        for (int i = 0; i < 8192; i++) heap[i] = (char)(i & 0x7F);
        long sum = 0;
        for (int i = 0; i < 8192; i++) sum += heap[i];
        choco_print("heap touched, checksum=");
        print_num(sum);
        choco_print("\n");
    }

    // File smoke test: read our own motd through open/read/close.
    long fd = choco_open("motd.txt");
    if (fd >= 0) {
        char buf[128];
        long n = choco_read((int)fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            choco_print("motd.txt says: ");
            choco_print(buf);
            if (buf[n - 1] != '\n') choco_print("\n");
        }
        choco_close(fd);
    } else {
        choco_print("(motd.txt not found)\n");
    }

    choco_print("Sleeping 500 ms...\n");
    choco_sleep(500);
    choco_print("Done. ticks=");
    print_num(choco_ticks());
    choco_print(". Bye!\n");
    return 0;
}

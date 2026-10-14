// /bin/ucat - print a file from user mode (open/read/close/lseek).
#include "lib.h"

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        choco_print("usage: ucat <path>\n");
        return 1;
    }
    long fd = choco_open(argv[1]);
    if (fd < 0) {
        choco_print("ucat: not found: ");
        choco_print(argv[1]);
        choco_print("\n");
        return 1;
    }
    char buf[256];
    for (;;) {
        long n = choco_read((int)fd, buf, sizeof(buf));
        if (n <= 0) break;
        choco_write(1, buf, (unsigned long)n);
    }
    choco_close(fd);
    return 0;
}

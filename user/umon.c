// /bin/umon - user-space system monitor (ring 3).
// The first app outside the kernel: reads everything from /proc,
// no privileged syscalls needed beyond open/read/close.
#include "lib.h"

static void show(const char *title, const char *path) {
    choco_print("== ");
    choco_print(title);
    choco_print(" ==\n");
    long fd = choco_open(path);
    if (fd < 0) {
        choco_print("(unavailable)\n");
        return;
    }
    char buf[256];
    for (;;) {
        long n = choco_read((int)fd, buf, sizeof(buf));
        if (n <= 0) break;
        choco_write(1, buf, (unsigned long)n);
    }
    choco_close(fd);
    choco_print("\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    show("version", "proc/version");
    show("meminfo", "proc/meminfo");
    show("uptime", "proc/uptime");
    show("threads", "proc/threads");
    return 0;
}

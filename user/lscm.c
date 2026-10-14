// SCM_RIGHTS fd passing over an AF_UNIX socketpair - the mechanism Wayland uses
// to hand the wl_shm fd to the compositor. Creates a memfd, writes a pattern,
// passes the fd with sendmsg, receives it with recvmsg, and proves the received
// descriptor maps the SAME shared pages (a write via the original, read via the
// received one).
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;

    int fd = (int)syscall(319, "pool", 0); // memfd_create
    if (fd < 3) return 1;
    if (ftruncate(fd, 4096) != 0) return 1;

    unsigned char *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED) return 1;
    a[0] = 0x41; a[1] = 0x42; a[2] = 0x43; a[3] = 0x44;

    char tx[2] = {'A', 'B'};
    struct iovec iv = { tx, 2 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iv;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    if (sendmsg(sv[0], &m, 0) != 2) return 1;

    char rx[2] = {0, 0};
    struct iovec iv2 = { rx, 2 };
    char cbuf2[CMSG_SPACE(sizeof(int))];
    struct msghdr m2;
    memset(&m2, 0, sizeof m2);
    m2.msg_iov = &iv2;
    m2.msg_iovlen = 1;
    m2.msg_control = cbuf2;
    m2.msg_controllen = sizeof cbuf2;
    long r = recvmsg(sv[1], &m2, 0);
    if (r != 2 || rx[0] != 'A' || rx[1] != 'B') return 1;

    struct cmsghdr *c2 = CMSG_FIRSTHDR(&m2);
    if (!c2 || c2->cmsg_level != SOL_SOCKET || c2->cmsg_type != SCM_RIGHTS) return 1;
    int nfd = -1;
    memcpy(&nfd, CMSG_DATA(c2), sizeof nfd);
    if (nfd < 3) return 1;

    unsigned char *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, nfd, 0);
    if (b == MAP_FAILED) return 1;
    // The received descriptor must map the same pages as the original.
    if (b[0] != 0x41 || b[1] != 0x42 || b[3] != 0x44) return 1;
    b[10] = 0x7E;
    if (a[10] != 0x7E) return 1;

    write(1, "scm: PASS\n", 10);
    return 0;
}

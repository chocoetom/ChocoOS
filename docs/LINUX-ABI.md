# ChocoOS Linux x86-64 ABI

What a statically linked Linux/x86-64 program may rely on when it runs on
ChocoOS. The translator lives in `kernel/proc/syscall.cpp` (`linux_dispatch`)
plus the helpers in the `choco::linux` / `choco::proc` namespaces. Numbers are
the Linux x86-64 syscall numbers; the entry stub is `kernel/cpu/linux.asm`
(System V register convention: rdi, rsi, rdx, r10, r8, r9), errors are
`-errno` like Linux.

Status legend: **OK** = implemented, **PART** = implemented with limits,
**STUB** = accepted but a no-op / fake success, **ENOSYS** = returns `-38`.

## Syscalls

| nr | name | status | notes |
|----|------|--------|-------|
| 0 | read | OK | fd 0 = console tty (line discipline); files/pipes otherwise |
| 1 | write | OK | fd 1/2 = console tty (OPOST/IXON); files/pipes otherwise |
| 3 | close | OK | closes fd 0-2 or an open slot |
| 4 | stat | OK | path-based, read-only meta (S_IFREG/S_IFDIR) |
| 5 | fstat | OK | `statbuf` in RSI; size from the descriptor |
| 6 | lstat | OK | same as `stat` (no symlinks) |
| 7 | poll | OK | POLLIN/POLLOUT on tty, sockets, pipes, files; timeout honoured |
| 8 | lseek | OK | SEEK_SET/CUR/END |
| 9 | mmap | OK | anonymous **and** file-backed MAP_PRIVATE (copies the file, zero past EOF) |
| 10 | mprotect | PART | applies R/W on the PTEs; **no NX yet** (EXEC maps readable) |
| 11 | munmap | OK | frees the anonymous arena |
| 12 | brk | OK | monotonic break (sbrk backend) |
| 13 | rt_sigaction | PART | per-process handlers; SIGKILL/SIGSTOP rejected |
| 14 | rt_sigprocmask | OK | SIG_BLOCK/UNBLOCK/SETMASK |
| 15 | rt_sigreturn | OK | restores the frame installed by a handler |
| 16 | ioctl | PART | terminal ioctls only, fd 0-2 (see below) |
| 19 | readv | OK | ≤32 iovecs |
| 20 | writev | OK | ≤32 iovecs |
| 21 | access | OK | path existence/dir check (no mode bits) |
| 22 | pipe | OK | anonymous pipes, buffer in `kernel/proc/syscall.cpp` |
| 23 | select | OK | fd_set (128 B); readfds/writefds; NULL timeout = block |
| 24 | sched_yield | OK | |
| 25 | gettimeofday | OK | real wall clock (CMOS RTC + uptime); `tv`/`tz` validated |
| 26 | settimeofday | STUB | accepted, ignored |
| 33 | dup2 | OK | |
| 35 | nanosleep | OK | based on PIT ticks |
| 39 | getpid | OK | |
| 56 | clone | PART | `CLONE_VM` supported; others ignored; no `CLONE_THREAD` |
| 59 | execve | OK | replaces the foreground image (`exec_current`) |
| 60 | exit | OK | |
| 61 | wait4 | OK | `status` is RSI; zombie/wait model |
| 62 | kill | PART | fatal sigs: 2/6/9/15; SIGCHLD(17) accepted |
| 63 | uname | OK | reports "Linux 5.4.0 ... x86_64" |
| 72 | fcntl | PART | minimal commands only |
| 82 | rename | OK | |
| 83 | mkdir | OK | |
| 84 | rmdir | OK | |
| 87 | unlink | OK | |
| 97 | getrlimit | STUB | returns RLIM_INFINITY |
| 102/104/107/108 | getuid/getgid/geteuid/getegid | STUB | always 0 |
| 109 | setpgid | OK | |
| 110 | getppid | OK | |
| 111 | getpgrp | OK | |
| 121 | getpgid | OK | |
| 158 | arch_prctl | OK | SET_FS / GET_FS (TLS base) |
| 200 | tkill | PART | alias of kill |
| 201 | time | OK | real wall clock (CMOS RTC + uptime) |
| 218 | set_tid_address | STUB | fake TID |
| 221 | fadvise64 | STUB | accepted |
| 228 | clock_gettime | OK | `CLOCK_REALTIME` from the RTC; other clocks are uptime |
| 231 | exit_group | OK | alias of exit |
| 234 | tgkill | PART | alias of kill |
| 257 | openat | OK | `AT_FDCWD` (`-100`) or a valid dirfd |
| 262 | newfstatat | OK | path-based stat |
| 273 | set_robust_list | STUB | accepted |
| 292 | dup3 | OK | flags must be 0 |
| 293 | pipe2 | OK | flags ignored |
| 318 | getrandom | PART | PIT-seeded, **not cryptographically secure**, ≤256 B |
| 334 | rseq | ENOSYS | musl tolerates it |
| 202 | futex | ENOSYS | no threads yet (see Fase 22) |

### Sockets

`socket`(41)/`connect`(42)/`sendto`(44)/`recvfrom`(45)/`shutdown`(48)/
`getsockname`(51)/`getpeername`(52)/`setsockopt`(54)/`getsockopt`(55) and the
**server side** `bind`(49)/`listen`(50)/`accept`(43)/`accept4`(288) are
implemented for `AF_INET` + `SOCK_STREAM` over the kernel TCP engine;
`read`/`write`/`close`/`poll` work on the fd. `socketpair`(53) supports
`AF_UNIX` + `SOCK_STREAM` (a connected pair over pipes). `AF_UNIX` **by path**
also works: `bind`(49) reserves a name, `listen`(50) makes it joinable and
`connect`(42)/`accept`(43) hand out connected endpoints backed by the same pipe
pair (the name is a kernel-side key only, no filesystem node is created).

### Not implemented (return `-ENOSYS`)

`epoll/eventfd`, `chdir/getcwd/fchdir`, `mremap`, `splice`, `statfs`,
`io_setup`, `signalfd`, `inotify`, `prlimit64`.

## Signals

- Delivered: `SIGINT(2)`, `SIGQUIT(3)`, `SIGTERM(15)`, `SIGKILL(9)` (fatal
  path), `SIGCHLD(17)` (accepted; the zombie/`wait4` model reports it).
- Raised by the tty discipline (ISIG): `SIGINT`, `SIGQUIT` (VINTR/VQUIT) and
  `SIGTSTP(20)` (VSUSP).
- Raised by the console on `TIOCSWINSZ`: `SIGWINCH(28)`.
- Delivery is **synchronous-ish**: a handled signal is queued and dispatched on
  the next syscall return (`proc::deliver_pending`); a blocked `read()` returns
  `-EINTR`. There is no delivery on interrupt return yet, so a task that never
  enters the kernel will not receive a queued handler.

## clone flags

| flag | value | status |
|------|-------|--------|
| CLONE_VM | 0x100 | OK (shared address space) |
| CLONE_THREAD | 0x10000 | **not supported** (futex/pthreads need it) |
| CLONE_SETTLS / CHILD_CLEARTID | | not supported |
| SIGCHLD low byte | 0x11 | the usual value seen in tests |

## Terminal ioctls (fd 0/1/2)

`TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF` (termios, 60 bytes),
`TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`, `FIONREAD`, `TIOCSTI`,
`TCXONC`, `TCFLSH`. See `kernel/include/choco/tty.hpp` for the request numbers
and the `struct termios` layout.

## Known limitations that break real programs

- `mmap` is anonymous-only → no `ld.so` / dynamic linking, no file `mmap`.
- `mprotect` does not enforce permissions → no `PROT_EXEC` JIT.
- No `futex` / `CLONE_THREAD` → no pthreads.
- Sockets: `AF_INET` TCP client **and** server side, plus `AF_UNIX` (`socketpair` and named paths). No `epoll`. `poll`/`select` work.
- No `chdir`/`getcwd`; `envp` is empty (no `PATH`/`HOME`/`TERM`).
- FP/SSE executes (CR0.EM=0, CR4.OSFXSR set) but FP state is **not**
  context-switched, so only single-threaded FP code is safe.

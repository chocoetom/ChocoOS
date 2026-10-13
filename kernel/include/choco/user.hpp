#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::syscall {

// Numbers (must match user/lib.h).
inline constexpr uint64_t N_EXIT = 0;
inline constexpr uint64_t N_WRITE = 1;
inline constexpr uint64_t N_READ = 2;
inline constexpr uint64_t N_SLEEP = 3;
inline constexpr uint64_t N_TICKS = 4;
inline constexpr uint64_t N_YIELD = 5;
inline constexpr uint64_t N_OPEN = 6;    // open(path, flags) -> fd >= 3
inline constexpr uint64_t N_CLOSE = 7;   // close(fd)
inline constexpr uint64_t N_SBRK = 8;    // sbrk(incr) -> old break (0=query)
inline constexpr uint64_t N_LSEEK = 9;   // lseek(fd, off, whence)
inline constexpr uint64_t N_FSIZE = 10;  // fsize(fd) -> size
inline constexpr uint64_t N_FSCOUNT = 11;// initrd entry count
inline constexpr uint64_t N_FSSTAT = 12; // fsstat(idx, name_buf, name_cap) -> size
inline constexpr uint64_t N_WIN_CREATE = 13; // win_create(w, h, title) -> uaddr
inline constexpr uint64_t N_WIN_PRESENT = 14;// win_present(uaddr) -> 0
inline constexpr uint64_t N_WIN_CLOSE = 15;  // win_close(uaddr) -> 0
inline constexpr uint64_t N_MKDIR = 16;      // mkdir(path)
inline constexpr uint64_t N_UNLINK = 17;     // unlink(path)
inline constexpr uint64_t N_RENAME = 18;     // rename(old, new)
inline constexpr uint64_t N_RMDIR = 19;      // rmdir(path)
inline constexpr uint64_t N_IOCTL = 20;      // ioctl(fd, request, arg)
inline constexpr uint64_t N_WIN_RESIZE = 21; // win_resize(uaddr, w, h)

struct Frame; // choco::cpu::Frame (forward to avoid the include loop)
void handle(uint64_t vec, void *frame_ptr);
void reset_files(); // clear the foreground open-file table (on run/exit)
void reset_files_for(uint32_t pid); // clear one task's descriptors (job exit)
void clone_files(uint32_t child_pid); // inherit descriptors for a new process
void drop_files(uint32_t pid); // close all descriptors when a child exits
void set_tty_pgid(uint32_t pgid);
// Install a redirect into a freshly spawned process's stdio (fd 1/2 to a
// writable, fd 0 from an in-memory file). Called by run_args_redir.
void set_std_out(uint32_t pid, long writable_id);
void set_std_in(uint32_t pid, const void *data, size_t size);
// Validates a user buffer in the CURRENT address space (low half, mapped).
bool user_ok(uint64_t ptr, uint64_t len);

} // namespace choco::syscall

namespace choco::elf {

struct Loaded {
    bool ok = false;
    uint64_t entry = 0;
    uint64_t phdr_vaddr = 0; // for AT_PHDR (Linux auxv)
    uint64_t phent = 0, phnum = 0;
    // Dynamic linking
    const char *interp_path = nullptr; // PT_INTERP path (null-terminated in initrd)
    uint64_t dynamic_vaddr = 0;        // PT_DYNAMIC segment virtual address
    uint64_t phdr_relative = 0;        // for PIE: phdr offset relative to image base
};

// Minimal ELF64 loader: maps every PT_LOAD with USER|RW, copies file data,
// zeroes BSS. Returns the entry point on success.
Loaded load(const void *file, size_t len);
// Same, but into an explicit address space (no reliance on the current
// CR3: copies go through the HHDM). brk_out receives the first address
// after the last LOAD segment (page-aligned, 0 on failure path).
Loaded load_as(uint64_t root_phys, const void *file, size_t len,
               uint64_t &brk_out);

} // namespace choco::elf

namespace choco::fs {

// Tiny read-only FS over the initrd.tar boot module (USTAR).
void init(); // parses the first boot module; empty FS when absent
size_t count(); // initrd files + procfs virtual files
bool stat(size_t i, const char **name_out, size_t *size_out);
// Path may start with '/'. Falls through to procfs (proc/*). False when missing.
bool read(const char *path, const void **data_out, size_t *size_out);
long writable_open(const char *path, bool create, bool truncate,
                   const uint8_t **data_out, size_t *size_out);
long writable_write(long id, size_t off, const void *src, size_t len, bool append,
                    size_t *new_off);
bool writable_unlink(const char *path);
bool writable_rename(const char *old_path, const char *new_path);
bool writable_mkdir(const char *path);
bool writable_rmdir(const char *path);
bool writable_is_dir(const char *path);
// Per-mount inventory, for /proc/choco and `df`.
struct Summary {
    size_t initrd = 0;         // files baked into the boot module
    size_t writable = 0;       // live files in the RAM overlay
    size_t writable_bytes = 0; // their content size
    size_t fat = 0;            // entries on the FAT32 volume
    size_t proc = 0;           // virtual files
    uint64_t fat_bytes = 0;    // mounted volume size
    uint64_t fat_free = 0;     // unallocated clusters
    bool fat_mounted = false, fat_rw = false;
};
Summary summary();
// Optional persistent volume on the secondary IDE channel (see fs.cpp).
// Both are safe to call with no disk attached: nothing is written and the
// in-memory VFS keeps working.
long disk_persistence_init();
void disk_persistence_flush();

} // namespace choco::fs

namespace choco::procfs {

// Linux-style virtual files (proc/version, proc/meminfo, proc/uptime,
// proc/threads), served through fs::read/stat/count with no extra syscalls.
size_t count();
bool stat(size_t i, const char **name_out, size_t *size_out);
bool read(const char *path, const void **data_out, size_t *size_out);

} // namespace choco::procfs

namespace choco::linux {

// Address-space helpers for the Linux translator (per foreground process).
void init();  // EFER.SCE + STAR/LSTAR/FMASK; call once at boot
void reset(); // clear mmap arena + FS base on every run
void reset_for(uint32_t pid); // clear a specific process's arena (job setup)
void switch_fs(uint32_t pid); // load a process's FS base on a context switch
uint64_t mmap(uint64_t hint, uint64_t len); // anonymous private
// File-backed private mapping: copies src[off .. off+len) into fresh pages
// (zero past EOF). Used by ld.so-style loaders.
uint64_t mmap_file(uint64_t hint, uint64_t len, const void *src, size_t src_len,
                   uint64_t off);
long mprotect(uint64_t addr, uint64_t len, uint64_t prot); // PROT_R/W/X/NONE
long munmap(uint64_t addr, uint64_t len);
long brk(uint64_t addr); // absolute break, monotonic
long set_fs(uint64_t addr); // ARCH_SET_FS (+WRMSR immediately)
uint64_t get_fs();

} // namespace choco::linux

namespace choco::proc {

// Load path (ELF) from the initrd, spawn it as a ring-3 thread and wait
// until it exits (or Ctrl+C kills it). Returns its exit code, or -1 on error.
long run_file(const char *path);
long run_args(const char *path, int argc, char **argv);
// Same, but with stdio redirection: out_path receives stdout+stderr (append
// when `append`), in_path feeds stdin. Either may be null.
long run_args_redir(const char *path, int argc, char **argv, const char *out_path,
                    bool append, const char *in_path);
// Launch a program as a detached job and return at once: the WM keeps pumping
// the desktop while the app runs, and several programs coexist. Returns 0 (and
// the pid) on success, -1 on failure. No console foreground state is touched.
long spawn_job(const char *path, int argc, char **argv, uint32_t *pid_out);
// Reap finished jobs (called from the ambient GUI loop). Frees their address
// space, descriptors and thread slot.
void reap_jobs();
// Terminate a job by pid (WM window close button). False if it is not a live job.
bool kill_job(uint32_t pid);
bool foreground_active();
// Called by the exit syscall; never returns.
[[noreturn]] void exit_current(long code);
long linux_sigaction(uint64_t sig, uint64_t act, uint64_t oldact);
long linux_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset);
bool linux_signal_ignored(uint64_t sig);
bool linux_deliver_signal(uint64_t sig, void *frame);
long linux_sigreturn(void *frame);
// Break management for the foreground process (sbrk syscall backend).
uint64_t proc_brk();
// Program name (basename of the path that was run) and heap growth beyond the
// break the loader needed. Back the /proc process table; zero without a
// foreground task.
const char *proc_comm();
uint64_t proc_data_bytes();
uint64_t proc_sbrk(int64_t incr); // old break, (uint64_t)-1 on OOM
uint64_t proc_root();             // user PML4 of the CALLING process
uint32_t proc_pid();
uint32_t proc_ppid();
// The console foreground job (the one run_args launched): used by /proc to
// label and size the job that owns the display.
uint32_t foreground_pid();
const char *foreground_comm();
uint64_t foreground_data_bytes();
long exec_current(uint64_t path, uint64_t argv, void *frame_ptr);
long clone_current(uint64_t flags, uint64_t child_stack, void *frame_ptr);
// Asynchronous signal from the tty discipline (ISIG, SIGWINCH, SIGCHLD).
// Delivers to the tty group's handlers, or terminates a default-fatal signal.
void raise_signal(uint32_t sig);
// Force the foreground program to terminate from the WM (window close button);
// a user handler cannot defer this, the job is torn down unconditionally.
void kill_foreground();
// Called on the syscall return path: dispatches any deferred signal a handler
// registered for. Safe to call with no pending signal.
void deliver_pending(void *frame_ptr);
// True when a handled signal is queued: a blocked read() should return EINTR.
bool has_pending();

} // namespace choco::proc

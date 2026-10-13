// FAT32 driver: ramdisk (read-only) or ATA secondary volume (read/write).
//   BPB -> geometry -> FAT cluster chain walk -> 8.3 + LFN directory entries.
// The whole tree is flattened into paths "disk/..." — the same convention
// procfs/initrd use — so fs:: needs no new syscalls.
#pragma once
#include <choco/vfs.hpp>
#include <stdint.h>
#include <stddef.h>

namespace choco::fs::fat32 {

// Mount the Limine boot module image (read-only).
bool mount_ramdisk(const uint8_t *image, size_t size);
// Mount the persistent ATA volume on the secondary channel (read/write).
// Returns false when no disk answers; the caller falls back to the module.
bool mount_ata();

bool mounted();
bool writable();               // true when backed by the ATA volume

// Enumeration/lookup used by fs::
size_t count();
bool stat(size_t i, const char **name_out, size_t *size_out);
bool read(const char *path, const void **data_out, size_t *size_out);
bool is_dir(const char *path);

// Write ops (ATA backend only; they return false on the read-only module).
// Paths accept the full flattened form ("disk/docs/x.txt").
bool create(const char *path);
bool write_file(const char *path, const void *data, size_t size);
bool unlink(const char *path);
bool mkdir(const char *path);
bool rename(const char *old_path, const char *new_path);

// --- telemetry for /proc/diskstats ---------------------------------------
struct IoStats {
    bool disk = false;            // an ATA device answered IDENTIFY
    uint64_t sectors = 0;         // device size, 512-byte sectors
    uint64_t reads = 0, writes = 0;
    uint64_t read_sectors = 0, write_sectors = 0;
    uint64_t errors = 0;          // transfers that never reached DRQ
};
IoStats io_stats();
uint64_t volume_bytes();  // size of the mounted FAT32 volume (0 when unmounted)
// Unallocated clusters, scanned from the in-memory FAT copy.
uint64_t free_bytes();
bool on_ata();            // mounted from the disk rather than the boot module

} // namespace choco::fs::fat32

// VFS - Virtual Filesystem abstraction layer
// Supports multiple filesystem types (FAT32, initrd, procfs, etc.)
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace choco::vfs {

// File types
enum class FileType : uint8_t {
    REGULAR,
    DIRECTORY,
    SYMLINK,
    DEVICE,
    PIPE,
    SOCKET,
    UNKNOWN
};

// File metadata
struct Stat {
    const char *name;
    uint64_t size;
    uint64_t inode;
    uint8_t  mode;
    FileType type;
    uint64_t mtime;  // modification time
    uint64_t atime;  // access time
    uint64_t ctime;  // change time
    uint32_t uid;
    uint32_t gid;
};

// Directory entry for listing
struct DirEnt {
    uint64_t inode;
    uint64_t offset;
    uint16_t rec_len;
    FileType type;
    char name[256];
};

// VFS Node - represents a file/directory in the system
struct Node {
    const char *name;
    uint64_t size;
    FileType type;
    uint32_t flags;

    // File operations
    bool (*read)(Node *node, void *buf, size_t size, size_t offset);
    bool (*write)(Node *node, const void *buf, size_t size, size_t offset);
    bool (*close)(Node *node);
    bool (*readdir)(Node *node, DirEnt *ent, size_t *count);
    bool (*stat)(Node *node, Stat *st);
};

// Mount point structure
struct Mount {
    const char *path;
    Node *root;
    Mount *next;
};

// VFS Mount Operations
struct MountOps {
    bool (*mount)(Mount *mnt);
    bool (*umount)(Mount *mnt);
    bool (*statfs)(Mount *mnt, void *buf);
};

// Registry
bool register_fs(const char *name, MountOps *ops);
Mount *mount(const char *path, const char *fs_name, void *dev);
bool umount(const char *path);

// Utility
Node *create_node(const char *name, FileType type);
void destroy_node(Node *node);
Node *vfs_lookup(const char *path);

// Helper
bool vfs_mount(const char *path, const char *fs_name, void *dev);
bool vfs_umount(const char *path);

// Global VFS state
extern Mount *g_mounts;

} // namespace choco::vfs
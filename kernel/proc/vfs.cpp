// VFS implementation - Virtual File System
// Manages mount points and filesystem abstractions
#include <choco/vfs.hpp>
#include <choco/mem.hpp>
#include <choco/console.hpp>
#include <choco/util.hpp>

namespace choco::vfs {

Mount *g_mounts = nullptr;

// Registry for filesystem types
bool register_fs(const char *name, MountOps *ops) {
    (void)name; (void)ops;
    console::kprintf(0xFFFFFF, 0x000000, "[vfs] register_fs('%s')\n", name);
    return true; // stub for now
}

// Create a new node
Node *create_node(const char *name, FileType type) {
    Node *node = (Node *)choco::heap::alloc(sizeof(Node));
    if (!node) return nullptr;

    node->name = name;
    node->size = 0;
    node->type = type;
    node->flags = 0;

    node->read = nullptr;
    node->write = nullptr;
    node->close = nullptr;
    node->readdir = nullptr;
    node->stat = nullptr;

    return node;
}

void destroy_node(Node *node) {
    if (node) choco::heap::free(node);
}

// Mount a filesystem
bool vfs_mount(const char *path, const char *fs_name, void *dev) {
    (void)dev;
    Mount *mnt = (Mount *)choco::heap::alloc(sizeof(Mount));
    if (!mnt) return false;

    mnt->path = path;
    mnt->root = create_node(path, FileType::DIRECTORY);
    mnt->next = nullptr;

    if (g_mounts) mnt->next = g_mounts;
    g_mounts = mnt;

    console::kprintf(0x00FF00, 0x000000, "[vfs] mounted %s to %s\n", fs_name, path);
    return true;
}

// Unmount a filesystem
bool vfs_umount(const char *path) {
    Mount **prev = &g_mounts;
    for (Mount *m = g_mounts; m; m = m->next) {
        if (util::strcmp(m->path, path) == 0) {
            *prev = m->next;
            destroy_node(m->root);
            choco::heap::free(m);
            console::kprintf(0x00FF00, 0x000000, "[vfs] unmounted %s\n", path);
            return true;
        }
        prev = &m->next;
    }
    return false;
}

} // namespace choco::vfs
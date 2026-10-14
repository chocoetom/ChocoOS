#pragma once
#include <stddef.h>
#include <stdint.h>

// Anonymous memory objects, the backing store for Wayland wl_shm pools (and
// anything else that needs a file you can mmap MAP_SHARED and resize). One
// object is a lazily-grown array of physical pages; descriptors reference it by
// id, and MAP_SHARED mappings of the same id across processes share pages.
namespace choco::memfd {

// Create a new object; returns its id (>= 0) or a negative errno.
int create();
// Reference counting for descriptors that alias the object (dup / SCM_RIGHTS).
void ref(int id);
void unref(int id);

// Resize to len bytes (rounded up to a page), zero-filling new pages.
bool truncate(int id, uint64_t len);
// Current size in bytes (0 for an invalid id).
uint64_t size(int id);

// Physical address of page `pageno` (allocated on demand), or 0.
uint64_t page_phys(int id, uint64_t pageno);
// Number of pages currently backing the object.
uint64_t page_count(int id);

// Byte-level I/O used by read()/write() on a memfd descriptor.
size_t write_at(int id, uint64_t off, const void *src, size_t len);
size_t read_at(int id, uint64_t off, void *dst, size_t len);

} // namespace choco::memfd

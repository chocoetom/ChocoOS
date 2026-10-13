#pragma once
#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer_response;
struct limine_memmap_response;
struct limine_file;

namespace choco::boot {

struct Info {
    bool ok = false;
    void *fb_addr = nullptr;
    uint64_t fb_w = 0, fb_h = 0, fb_pitch_px = 0;
    uint64_t fb_phys = 0; // for paging to map (when available)
    limine_memmap_response *memmap = nullptr;
    uint64_t hhdm = 0;
    uint64_t kern_phys = 0, kern_virt = 0, kern_len = 0;
    // Boot modules (initrd.tar is expected as the first one).
    limine_file **modules = nullptr;
    uint64_t module_count = 0;
};

// Collected once in kmain from the Limine requests.
const Info &info();
bool collect(); // false => halt

} // namespace choco::boot

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::paging {

inline constexpr uint64_t F_PRESENT = 1ULL << 0;
inline constexpr uint64_t F_RW      = 1ULL << 1;
inline constexpr uint64_t F_USER    = 1ULL << 2; // ring-3 reachable (syscalls, user stacks, ELF)
inline constexpr uint64_t F_HUGE    = 1ULL << 7; // PS: 2MB (PD) or 1GB (PDPT) page

void init(); // builds our own PML4, maps everything, switches CR3
uint64_t current_pml4_phys();
bool map(uint64_t virt, uint64_t phys, uint64_t flags);
bool unmap(uint64_t virt);
uint64_t translate(uint64_t virt); // 0 when unmapped
uint64_t hhdm();
void *phys_to_virt(uint64_t phys);

// Per-process address spaces (phase 7): a user root clones the kernel
// half (PML4[256..511]: HHDM + kernel image + kernel stacks) and leaves
// the low half empty. The scheduler switches CR3 on thread change.
uint64_t create_user_root(); // 0 = OOM
uint64_t clone_user_root(uint64_t root_phys); // deep-copy low-half user pages
void destroy_user_root(uint64_t root_phys); // frees low-half tables+frames
bool map_as(uint64_t root_phys, uint64_t virt, uint64_t phys, uint64_t flags);
bool unmap_as(uint64_t root_phys, uint64_t virt);
// Change the permission bits of an existing user page, keeping its frame
// (used by mprotect). `flags` are the low-12 PTE bits (F_PRESENT/F_RW/F_USER).
bool protect_as(uint64_t root_phys, uint64_t virt, uint64_t flags);
uint64_t translate_as(uint64_t root_phys, uint64_t virt); // 0 when unmapped
inline uint64_t read_cr3() { uint64_t v; asm volatile("mov %%cr3,%0" : "=r"(v)); return v; }
inline void write_cr3(uint64_t v) { asm volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }

} // namespace choco::paging

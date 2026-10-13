#pragma once
#include <stddef.h>
#include <stdint.h>
#include <choco/paging.hpp>

namespace choco::pmm {

inline constexpr uint64_t PAGE = 4096;

void init(); // reads the memmap via boot::info()
uint64_t alloc();            // returns a PHYSICAL address, 0 = out of memory
uint64_t alloc_zero();       // same, zeroed (via HHDM)
void free(uint64_t phys);

uint64_t total_bytes();
uint64_t free_bytes();
uint64_t used_bytes();
uint64_t total_pages();
uint64_t alloc_calls(); // diagnostics: how many alloc() calls so far
const char *memmap_type_name(uint64_t type);
void dump_map(); // prints the memmap entries

} // namespace choco::pmm

namespace choco::heap {

void init(); // maps the initial heap via pmm+paging
void *alloc(size_t size); // kmalloc
void free(void *p);       // kfree
void *realloc(void *p, size_t ns);

size_t mapped_bytes();
size_t used_bytes();
bool self_test(); // allocates/verifies/frees, true when OK

} // namespace choco::heap

// kmalloc/kfree C ABI for the new/delete operators and future drivers
extern "C" {
void *kmalloc(size_t size);
void kfree(void *p);
}

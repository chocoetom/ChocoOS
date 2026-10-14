// Limine requests + BootInfo collection. ONLY TU holding the requests.
#include <limine.h>
#include <choco/boot.hpp>

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request g_fb = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request g_mm = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request g_hhdm = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request g_kaddr = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request g_mod = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0, .response = nullptr,
    .internal_module_count = 0, .internal_modules = nullptr,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

namespace choco::boot {

static Info g_info{};

const Info &info() { return g_info; }

// Linker-script symbols (global scope: the script defines _kernel_start/end).
extern "C" { extern char _kernel_start[]; extern char _kernel_end[]; }

bool collect() {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) return false;
    if (g_fb.response == nullptr || g_fb.response->framebuffer_count < 1) return false;
    if (g_mm.response == nullptr || g_hhdm.response == nullptr) return false;

    auto *fb = g_fb.response->framebuffers[0];
    g_info.fb_addr = fb->address;
    g_info.fb_w = fb->width;
    g_info.fb_h = fb->height;
    g_info.fb_pitch_px = fb->pitch / 4;
    // Limine gives no stable direct phys here; paging covers it via the memmap.
    g_info.fb_phys = 0;
    g_info.memmap = (limine_memmap_response *)g_mm.response;
    g_info.hhdm = g_hhdm.response->offset;
    if (g_mod.response != nullptr) {
        g_info.modules = (limine_file **)g_mod.response->modules;
        g_info.module_count = g_mod.response->module_count;
    }
    if (g_kaddr.response != nullptr) {
        g_info.kern_phys = g_kaddr.response->physical_base;
        g_info.kern_virt = g_kaddr.response->virtual_base;
        g_info.kern_len = 0; // computed from the linker symbols below
    }
    // Symbols defined in linker.ld. Fallback: 4MB from the 1MB physical mark.
    uint64_t ks = (uint64_t)&_kernel_start;
    uint64_t ke = (uint64_t)&_kernel_end;
    if (ke > ks && (ke - ks) < 0x2000000) {
        uint64_t len = (ke - ks + 0xFFF) & ~0xFFFULL;
        if (g_kaddr.response != nullptr) {
            // Limine phys/virt are consistent; just fix up the real size.
            g_info.kern_len = len;
        } else {
            g_info.kern_virt = ks & ~0xFFFULL;
            g_info.kern_phys = 0x100000;
            g_info.kern_len = len;
        }
    } else if (g_info.kern_phys != 0) {
        g_info.kern_virt = g_info.kern_virt ? g_info.kern_virt : 0xFFFFFFFF80000000ULL;
        g_info.kern_len = 4 * 1024 * 1024;
    } else {
        g_info.kern_virt = 0xFFFFFFFF80000000ULL;
        g_info.kern_phys = 0x100000;
        g_info.kern_len = 4 * 1024 * 1024;
    }
    g_info.ok = true;
    return true;
}

} // namespace choco::boot

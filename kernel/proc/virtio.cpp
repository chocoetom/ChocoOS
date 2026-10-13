// virtio 1.0 modern PCI transport: correct common-cfg offsets, 64-bit BARs,
// MMIO mapping via HHDM, and a working split virtqueue.
#include <choco/virtio.hpp>
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/mem.hpp>
#include <choco/paging.hpp>
#include <choco/pci.hpp>
#include <choco/util.hpp>

using choco::console::BG;
using choco::console::FG;

namespace choco::virtio {
namespace {

inline uint32_t cc32(Device *dev, uint32_t off) {
    return *(volatile uint32_t *)(dev->common_cfg + off);
}
inline void cc32w(Device *dev, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(dev->common_cfg + off) = v;
}
inline uint16_t cc16(Device *dev, uint32_t off) {
    return *(volatile uint16_t *)(dev->common_cfg + off);
}
inline void cc16w(Device *dev, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(dev->common_cfg + off) = v;
}
inline void barrier() { asm volatile("" ::: "memory"); }

// Map [phys, phys+len) into HHDM so MMIO is reachable. HHDM already maps
// usable RAM; PCI BARs are not RAM, so map them on demand.
uint8_t *map_mmio(uint64_t phys, uint64_t len) {
    uint64_t base = phys & ~0xFFFULL;
    uint64_t end = (phys + len + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = base; p < end; p += 4096) {
        if (paging::translate(paging::hhdm() + p) == 0)
            paging::map(paging::hhdm() + p, p, paging::F_PRESENT | paging::F_RW);
    }
    return (uint8_t *)paging::phys_to_virt(phys);
}

// Read a BAR address handling 64-bit BARs. Sets *is_io.
uint64_t bar_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar, bool &is_io) {
    uint32_t lo = pci::read_config32(bus, slot, func, 0x10 + bar * 4);
    is_io = (lo & 1) != 0;
    if (is_io) return lo & 0xFFFFFFFCu;
    uint8_t type = (lo >> 1) & 3;
    if (type == 2) {  // 64-bit
        uint32_t hi = pci::read_config32(bus, slot, func, 0x10 + (bar + 1) * 4);
        return ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0u);
    }
    return lo & 0xFFFFFFF0u;
}

struct PciCap {
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t mult;
};

bool read_virtio_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t pos, uint8_t &type,
                     PciCap &out) {
    uint32_t h0 = pci::read_config32(bus, slot, func, pos);
    uint32_t h1 = pci::read_config32(bus, slot, func, pos + 4);
    uint32_t h2 = pci::read_config32(bus, slot, func, pos + 8);
    uint32_t h3 = pci::read_config32(bus, slot, func, pos + 12);
    uint8_t id = h0 & 0xFF;
    if (id != 0x09) return false;
    // Raw dwords (little-endian): h0 = vndr|next<<8|len<<16|cfg_type<<24,
    // h1 = bar|id<<8|pad<<16, h2 = offset, h3 = length.
    type = (h0 >> 24) & 0xFF;  // cfg_type is the 4th byte
    // Raw bytes: [vndr][next][len][cfg_type][bar][pad x3][offset][length][mult?]
    // h0 = vndr|next<<8|len<<16|cfg_type<<24
    uint8_t bar = h1 & 0xFF;
    uint32_t offset = h2;
    uint32_t length = h3;
    // notify multiplier lives in a second dword for NOTIFY caps (spec: cap
    // is 16 bytes when cfg_type==NOTIFY, extra u32 follows).
    uint32_t mult = 0;
    if (type == VIRTIO_PCI_CAP_NOTIFY_CFG)
        mult = pci::read_config32(bus, slot, func, pos + 16);
    out.bar = bar;
    out.offset = offset;
    out.length = length;
    out.mult = mult;
    return true;
}

}  // namespace

bool probe_device(Device *dev, uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci::read_config16(bus, slot, func, 0x00);
    uint16_t device = pci::read_config16(bus, slot, func, 0x02);
    if (vendor != 0x1AF4) return false;
    uint16_t virtio_id;
    // Transitional IDs 0x1000..0x103F (legacy+modern) and modern 0x1040..0x107F.
    // Transitional mapping is shifted by one: 0x1000 = network(1).
    if (device >= 0x1000 && device <= 0x103F)
        virtio_id = device - 0x1000 + 1;
    else if (device >= 0x1040 && device <= 0x107F)
        virtio_id = device - 0x1040;
    else
        return false;

    util::memset(dev, 0, sizeof(Device));
    dev->vendor_id = vendor;
    dev->device_id = virtio_id;
    dev->revision = pci::read_config8(bus, slot, func, 0x08);
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->irq = pci::read_config8(bus, slot, func, 0x3C);

    pci::enable_bus_master(bus, slot, func);

    uint64_t common_phys = 0, notify_phys = 0, isr_phys = 0, devcfg_phys = 0;
    uint64_t common_len = 0, notify_len = 0, isr_len = 0, devcfg_len = 0;
    uint32_t notify_mult = 0;
    uint8_t notify_bar = 0;
    bool notify_is_io = false;
    uint8_t common_bar = 0, isr_bar = 0, devcfg_bar = 0;

    uint8_t cap_ptr = pci::read_config8(bus, slot, func, 0x34);
    int guard = 0;
    while (cap_ptr && guard++ < 32) {
        uint8_t cap_id = pci::read_config8(bus, slot, func, cap_ptr);
        uint8_t cap_next = pci::read_config8(bus, slot, func, cap_ptr + 1);
        if (cap_id == 0x09) {
            uint8_t type = 0;
            PciCap c{};
            if (read_virtio_cap(bus, slot, func, cap_ptr, type, c)) {
                console::kprintf(FG, BG, "[virtio] cap type %u bar %u off 0x%x len 0x%x mult %u\n",
                                 type, c.bar, c.offset, c.length, c.mult);
                bool is_io = false;
                uint64_t base = bar_addr(bus, slot, func, c.bar, is_io);
                if (type == VIRTIO_PCI_CAP_COMMON_CFG) {
                    common_phys = base + c.offset;
                    common_len = c.length;
                    common_bar = c.bar;
                    (void)common_bar;
                } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    notify_phys = base + c.offset;
                    notify_len = c.length ? c.length : 0x1000;
                    notify_mult = c.mult;
                    notify_bar = c.bar;
                    notify_is_io = is_io;
                } else if (type == VIRTIO_PCI_CAP_ISR_CFG) {
                    isr_phys = base + c.offset;
                    isr_len = c.length ? c.length : 1;
                    isr_bar = c.bar;
                    (void)isr_bar;
                } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    devcfg_phys = base + c.offset;
                    devcfg_len = c.length ? c.length : 0x100;
                    devcfg_bar = c.bar;
                    (void)devcfg_bar;
                }
            }
            if (type == VIRTIO_PCI_CAP_NOTIFY_CFG)
                cap_ptr = cap_next ? cap_next : 0;  // notify cap is 16 bytes
            else
                cap_ptr = cap_next;
            // notify cap consumes 16 bytes but next pointer already accounts
            (void)notify_bar;
        } else {
            cap_ptr = cap_next;
        }
    }

    if (!common_phys || (!notify_phys && !notify_is_io)) {
        console::kprintf(FG, BG, "[virtio] missing caps for device %u\n", dev->device_id);
        return false;
    }

    dev->common_cfg = map_mmio(common_phys, common_len ? common_len : 0x1000);
    if (notify_is_io) {
        dev->notify_is_io = true;
        dev->notify_port = (uint32_t)notify_phys;
        dev->notify_base = nullptr;
    } else {
        dev->notify_is_io = false;
        dev->notify_phys = notify_phys;
        dev->notify_base = map_mmio(notify_phys, notify_len);
    }
    dev->notify_off_multiplier = notify_mult;
    if (isr_phys) dev->isr = map_mmio(isr_phys, isr_len);
    if (devcfg_phys) dev->device_cfg = map_mmio(devcfg_phys, devcfg_len);

    if (!dev->common_cfg || (!dev->notify_base && !dev->notify_is_io)) {
        console::kprintf(FG, BG, "[virtio] BAR mapping failed\n");
        return false;
    }

    console::kprintf(FG, BG, "[virtio] dev %u rev %u irq %u mult %u %s\n", dev->device_id,
                     dev->revision, dev->irq, dev->notify_off_multiplier,
                     dev->notify_is_io ? "io-notify" : "mmio-notify");
    return true;
}

void reset_device(Device *dev) { set_status(dev, 0); }

void set_status(Device *dev, uint8_t status) {
    dev->status = status;
    *(volatile uint8_t *)(dev->common_cfg + REG_DEV_STATUS) = status;
}

uint8_t get_status(Device *dev) { return *(volatile uint8_t *)(dev->common_cfg + REG_DEV_STATUS); }

uint64_t get_features(Device *dev) {
    cc32w(dev, REG_DEV_FEAT_SEL, 0);
    barrier();
    uint32_t lo = cc32(dev, REG_DEV_FEAT);
    cc32w(dev, REG_DEV_FEAT_SEL, 1);
    barrier();
    uint32_t hi = cc32(dev, REG_DEV_FEAT);
    return ((uint64_t)hi << 32) | lo;
}

bool negotiate_features(Device *dev, uint64_t wanted) {
    dev->features = get_features(dev);
    dev->driver_features = dev->features & wanted;
    cc32w(dev, REG_DRV_FEAT_SEL, 0);
    barrier();
    cc32w(dev, REG_DRV_FEAT, (uint32_t)dev->driver_features);
    cc32w(dev, REG_DRV_FEAT_SEL, 1);
    barrier();
    cc32w(dev, REG_DRV_FEAT, (uint32_t)(dev->driver_features >> 32));
    set_status(dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    barrier();
    if (!(get_status(dev) & VIRTIO_STATUS_FEATURES_OK)) {
        console::kprintf(FG, BG, "[virtio] device rejected features\n");
        return false;
    }
    return true;
}

bool setup_queue(Device *dev, int qidx, uint16_t size) {
    if (qidx < 0 || qidx >= 4) return false;
    cc16w(dev, REG_QUEUE_SEL, (uint16_t)qidx);
    barrier();
    // Disable in case it was left enabled
    cc16w(dev, REG_QUEUE_ENABLE, 0);
    barrier();
    uint16_t max = cc16(dev, REG_QUEUE_SIZE);
    if (max == 0) return false;
    if (size > max) size = max;
    // Desc/avail/used each need their own page: heap pages are virtually
    // contiguous but NOT physically contiguous, and the device needs exact
    // guest-physical addresses. pmm pages are page-aligned and zeroed.
    if ((size_t)size * 16 > 4096 || 6 + (size_t)size * 2 > 4096 ||
        6 + (size_t)size * 8 > 4096) {
        console::kprintf(FG, BG, "[virtio] q%u size %u too big for 1 page each\n", qidx,
                         size);
        return false;
    }
    uint64_t desc_p = pmm::alloc_zero();
    uint64_t avail_p = pmm::alloc_zero();
    uint64_t used_p = pmm::alloc_zero();
    if (!desc_p || !avail_p || !used_p) return false;
    void *desc = paging::phys_to_virt(desc_p);
    void *avail = paging::phys_to_virt(avail_p);
    void *used = paging::phys_to_virt(used_p);

    VirtQueue *vq = &dev->queues[qidx];
    vq->size = size;
    vq->avail_idx = 0;
    vq->last_used_idx = 0;
    vq->desc_virt = desc;
    vq->avail_virt = avail;
    vq->used_virt = used;
    vq->desc_phys = desc_p;
    // Build free list
    for (uint16_t i = 0; i < size; i++) {
        uint8_t *d = (uint8_t *)desc + i * 16;
        // next = i+1
        *(volatile uint16_t *)(d + 14) = (i + 1 < size) ? (uint16_t)(i + 1) : 0xFFFF;
        *(volatile uint16_t *)(d + 12) = 0;  // flags
    }
    vq->free_head = 0;
    vq->num_free = size;

    cc16w(dev, REG_QUEUE_SEL, (uint16_t)qidx);
    barrier();
    cc16w(dev, REG_QUEUE_SIZE, size);
    barrier();
    *(volatile uint64_t *)(dev->common_cfg + REG_QUEUE_DESC) = desc_p;
    *(volatile uint64_t *)(dev->common_cfg + REG_QUEUE_AVAIL) = avail_p;
    *(volatile uint64_t *)(dev->common_cfg + REG_QUEUE_USED) = used_p;
    barrier();
    uint16_t noff = cc16(dev, REG_QUEUE_NOTIFY_OFF);
    vq->notify_off = noff;
    cc16w(dev, REG_QUEUE_ENABLE, 1);
    barrier();

    if (qidx + 1 > dev->num_queues) dev->num_queues = qidx + 1;
    console::kprintf(FG, BG, "[virtio] q%u size %u noff %u desc=0x%lx avail=0x%lx used=0x%lx\n",
                     qidx, size, noff, desc_p, avail_p, used_p);
    return true;
}

void notify_queue(Device *dev, int qidx) {
    uint16_t noff = dev->queues[qidx].notify_off;
    uint64_t off = (uint64_t)noff * dev->notify_off_multiplier;
    barrier();
    if (dev->notify_is_io) {
        cpu::outw((uint16_t)(dev->notify_port + off), (uint16_t)qidx);
    } else {
        *(volatile uint16_t *)(dev->notify_base + off) = (uint16_t)qidx;
        // DEBUG: also kick via the transitional legacy IO doorbell (BAR0
        // 0xc000 + QUEUE_NOTIFY 16). Remove once modern routing is confirmed.
        cpu::outw(0xc000 + 16, (uint16_t)qidx);
    }
    barrier();
}

void debug_queue(Device *dev, int qidx) {
    cc16w(dev, REG_QUEUE_SEL, (uint16_t)qidx);
    barrier();
    uint16_t sz = cc16(dev, REG_QUEUE_SIZE);
    uint16_t en = cc16(dev, REG_QUEUE_ENABLE);
    uint32_t dlo = *(volatile uint32_t *)(dev->common_cfg + REG_QUEUE_DESC);
    uint32_t dhi = *(volatile uint32_t *)(dev->common_cfg + REG_QUEUE_DESC + 4);
    uint32_t alo = *(volatile uint32_t *)(dev->common_cfg + REG_QUEUE_AVAIL);
    uint32_t ahi = *(volatile uint32_t *)(dev->common_cfg + REG_QUEUE_AVAIL + 4);
    uint32_t ulo = *(volatile uint32_t *)(dev->common_cfg + REG_QUEUE_USED);
    uint32_t uhi = *(volatile uint32_t *)(dev->common_cfg + REG_QUEUE_USED + 4);
    console::kprintf(FG, BG, "[virtio] q%u readback size=%u en=%u desc=0x%x:%x avail=0x%x:%x used=0x%x:%x\n",
                     qidx, sz, en, dhi, dlo, ahi, alo, uhi, ulo);
}

int virtqueue_add(Device *dev, int qidx, uint64_t buf_phys, uint32_t len, uint16_t flags) {
    if (!dev || qidx < 0 || qidx >= 4) return -1;
    VirtQueue *vq = &dev->queues[qidx];
    if (vq->size == 0 || vq->num_free == 0 || len == 0) return -1;
    uint16_t idx = vq->free_head;
    if (idx >= vq->size) return -1;
    uint8_t *d = (uint8_t *)vq->desc_virt + (size_t)idx * 16;
    uint16_t next = *(volatile uint16_t *)(d + 14);
    // Fill descriptor: addr(8) len(4) flags(2) next(2)
    *(volatile uint64_t *)(d + 0) = buf_phys;
    *(volatile uint32_t *)(d + 8) = len;
    *(volatile uint16_t *)(d + 12) = flags;
    *(volatile uint16_t *)(d + 14) = 0;
    vq->free_head = next;
    vq->num_free--;

    // Append to avail ring
    volatile uint16_t *avail_flags = (volatile uint16_t *)vq->avail_virt;
    volatile uint16_t *avail_idx_p = (volatile uint16_t *)((uint8_t *)vq->avail_virt + 2);
    volatile uint16_t *ring = (volatile uint16_t *)((uint8_t *)vq->avail_virt + 4);
    (void)avail_flags;
    uint16_t slot = vq->avail_idx % vq->size;
    ring[slot] = idx;
    barrier();
    vq->avail_idx++;
    *avail_idx_p = vq->avail_idx;
    barrier();
    return idx;
}

int virtqueue_reap(Device *dev, int qidx, uint32_t *len) {
    if (!dev || qidx < 0 || qidx >= 4) return -1;
    VirtQueue *vq = &dev->queues[qidx];
    if (vq->size == 0) return -1;
    volatile uint16_t *used_idx_p = (volatile uint16_t *)((uint8_t *)vq->used_virt + 2);
    uint16_t used_idx = *used_idx_p;
    barrier();
    if (used_idx == vq->last_used_idx) return -1;  // nothing completed
    // used ring entries: {id u32, len u32} each 8 bytes after 4-byte header
    uint8_t *ring = (uint8_t *)vq->used_virt + 4;
    uint16_t slot = vq->last_used_idx % vq->size;
    uint32_t id = *(volatile uint32_t *)(ring + slot * 8);
    uint32_t l = *(volatile uint32_t *)(ring + slot * 8 + 4);
    vq->last_used_idx++;
    if (len) *len = l;
    if (id >= vq->size) return -1;
    return (int)id;
}

void virtqueue_free(Device *dev, int qidx, uint16_t desc_idx) {
    if (!dev || qidx < 0 || qidx >= 4) return;
    VirtQueue *vq = &dev->queues[qidx];
    if (desc_idx >= vq->size) return;
    uint8_t *d = (uint8_t *)vq->desc_virt + (size_t)desc_idx * 16;
    *(volatile uint16_t *)(d + 14) = vq->free_head;
    vq->free_head = desc_idx;
    vq->num_free++;
}

// Compatibility shims (unused by the new net stack, kept correct-ish).
bool virtqueue_add_buf(VirtQueue *, const void *, uint32_t, bool) { return false; }
int virtqueue_get_buf(VirtQueue *, uint32_t *) { return -1; }

void init_device(Device *dev) {
    // queues[] is inline now; just clear counts.
    dev->num_queues = 0;
    dev->status = 0;
}

}  // namespace choco::virtio

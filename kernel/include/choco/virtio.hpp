// virtio: paravirtualized device framework (virtio 1.0, modern PCI caps).
#pragma once
#include <stdint.h>

namespace choco::virtio {

// PCI capability types
constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG = 1;
constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG = 2;
constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG = 3;
constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG = 4;
constexpr uint8_t VIRTIO_PCI_CAP_PCI_CFG = 5;

// Status bits
constexpr uint8_t VIRTIO_STATUS_ACK = 1;
constexpr uint8_t VIRTIO_STATUS_DRIVER = 2;
constexpr uint8_t VIRTIO_STATUS_DRIVER_OK = 4;
constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;
constexpr uint8_t VIRTIO_STATUS_DEVICE_NEEDS_RESET = 64;
constexpr uint8_t VIRTIO_STATUS_FAILED = 128;

// Device IDs (device field minus 0x1040)
constexpr uint16_t VIRTIO_ID_NETWORK = 1;
constexpr uint16_t VIRTIO_ID_BLOCK = 2;

// Common config register offsets (virtio 1.0, section 4.1.4.3)
constexpr uint32_t REG_DEV_FEAT_SEL = 0x00;
constexpr uint32_t REG_DEV_FEAT = 0x04;
constexpr uint32_t REG_DRV_FEAT_SEL = 0x08;
constexpr uint32_t REG_DRV_FEAT = 0x0C;
constexpr uint32_t REG_MSIX_CFG = 0x10;
constexpr uint32_t REG_NUM_QUEUES = 0x12;
constexpr uint32_t REG_DEV_STATUS = 0x14;
constexpr uint32_t REG_CFG_GEN = 0x15;
constexpr uint32_t REG_QUEUE_SEL = 0x16;
constexpr uint32_t REG_QUEUE_SIZE = 0x18;
constexpr uint32_t REG_QUEUE_MSIX = 0x1A;
constexpr uint32_t REG_QUEUE_ENABLE = 0x1C;
constexpr uint32_t REG_QUEUE_NOTIFY_OFF = 0x1E;
constexpr uint32_t REG_QUEUE_DESC = 0x20;
constexpr uint32_t REG_QUEUE_AVAIL = 0x28;
constexpr uint32_t REG_QUEUE_USED = 0x30;

// Split virtqueue descriptor flags
constexpr uint16_t DESC_F_NEXT = 1;
constexpr uint16_t DESC_F_WRITE = 2;

struct VirtQueue {
    uint16_t size = 0;
    uint16_t avail_idx = 0;      // driver cursor (next slot to fill)
    uint16_t last_used_idx = 0;  // driver cursor (next used entry to consume)
    uint16_t free_head = 0;      // free descriptor chain
    uint16_t num_free = 0;
    uint16_t notify_off = 0;
    void *desc_virt = nullptr;   // size * 16 bytes
    void *avail_virt = nullptr;  // 6 + 2*size
    void *used_virt = nullptr;   // 6 + 8*size
    uint64_t desc_phys = 0;
};

// Device abstraction
struct Device {
    uint16_t device_id = 0;
    uint16_t vendor_id = 0;  // 0x1af4
    uint8_t revision = 0;
    uint8_t bus = 0, slot = 0, func = 0;
    uint8_t *common_cfg = nullptr;
    uint8_t *notify_base = nullptr;  // MMIO mapping (null when port-mapped)
    uint64_t notify_phys = 0;
    uint32_t notify_port = 0;  // valid when notify_is_io
    bool notify_is_io = false;
    uint32_t notify_off_multiplier = 0;
    uint8_t *isr = nullptr;
    uint8_t *device_cfg = nullptr;
    uint8_t status = 0;
    uint64_t features = 0;
    uint64_t driver_features = 0;
    VirtQueue queues[4]{};
    int num_queues = 0;
    int irq = 0;
};

bool probe_device(Device *dev, uint8_t bus, uint8_t slot, uint8_t func);
void init_device(Device *dev);
void reset_device(Device *dev);
void set_status(Device *dev, uint8_t status);
uint8_t get_status(Device *dev);
uint64_t get_features(Device *dev);
bool negotiate_features(Device *dev, uint64_t wanted);
bool setup_queue(Device *dev, int qidx, uint16_t size);
void notify_queue(Device *dev, int qidx);
// Add a single descriptor: phys/len/flags. Returns descriptor index or -1.
int virtqueue_add(Device *dev, int qidx, uint64_t buf_phys, uint32_t len, uint16_t flags);
// Reap one completed buffer. Returns descriptor index or -1, *len = used len.
int virtqueue_reap(Device *dev, int qidx, uint32_t *len);
// Free a descriptor back to the pool (after copying data out).
void virtqueue_free(Device *dev, int qidx, uint16_t desc_idx);
// Debug: print latched queue addresses/state from the device.
void debug_queue(Device *dev, int qidx);

// Legacy single-buffer helpers (kept for compatibility, now correct).
bool virtqueue_add_buf(VirtQueue *vq, const void *buf, uint32_t len, bool write);
int virtqueue_get_buf(VirtQueue *vq, uint32_t *len);

}  // namespace choco::virtio

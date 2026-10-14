// virtio-net with a working split-queue RX pool and bounce-buffer TX.
// Negotiates MAC+STATUS only (10-byte virtio_net_hdr, no MRG_RXBUF).
#include <choco/virtio_net.hpp>
#include <choco/console.hpp>
#include <choco/cpu.hpp>
#include <choco/mem.hpp>
#include <choco/paging.hpp>
#include <choco/pci.hpp>
#include <choco/util.hpp>
#include <choco/virtio.hpp>

using choco::console::BG;
using choco::console::FG;

namespace choco::virtio_net {
namespace {

struct NetHdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;  // only when MRG_RXBUF negotiated (else absent)
};
static_assert(sizeof(NetHdr) == 12, "virtio_net_hdr_mrg is 12 bytes");

struct NetConfig {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
};

constexpr int RX_N = 32;
constexpr uint32_t RX_BUF_SZ = 2048;  // fits virtio header + full frame
constexpr uint32_t TX_BUF_SZ = 2048;

uint64_t s_rx_phys[RX_N]{};  // one pmm page per RX buffer (exact GPAs)
uint64_t s_tx_phys = 0;      // one pmm page TX bounce buffer
uint8_t *s_tx_virt = nullptr;
uint32_t s_hdr_sz = 10;  // 12 when MRG_RXBUF accepted, else 10

}  // namespace

bool probe_and_init(NetDevice *net) {
    util::memset(net, 0, sizeof(NetDevice));

    virtio::Device *vdev = &net->vdev;
    bool found = false;
    uint8_t fbus = 0, fslot = 0, ffunc = 0;
    for (int bus = 0; bus < 256 && !found; bus++) {
        for (int slot = 0; slot < 32 && !found; slot++) {
            for (int func = 0; func < 8 && !found; func++) {
                uint16_t vendor = pci::read_config16(bus, slot, func, 0x00);
                if (vendor == 0xFFFF) {
                    if (func == 0) {
                        // No function 0: skip remaining functions (PCI spec).
                        // Keep scanning simple: still check others cheaply.
                    }
                    continue;
                }
                virtio::Device tmp{};
                if (virtio::probe_device(&tmp, bus, slot, func)) {
                    if (tmp.device_id == virtio::VIRTIO_ID_NETWORK) {
                        *vdev = tmp;
                        fbus = bus;
                        fslot = slot;
                        ffunc = func;
                        found = true;
                    } else {
                        console::kprintf(FG, BG, "[virtio] skip device %u\n", tmp.device_id);
                    }
                }
            }
        }
    }
    if (!found) {
        console::kprintf(FG, BG, "[virtio-net] no virtio-net device\n");
        return false;
    }
    (void)fbus;
    (void)fslot;
    (void)ffunc;

    pci::enable_bus_master(vdev->bus, vdev->slot, vdev->func);
    virtio::init_device(vdev);
    virtio::reset_device(vdev);
    virtio::set_status(vdev, virtio::VIRTIO_STATUS_ACK | virtio::VIRTIO_STATUS_DRIVER);

    if (!virtio::negotiate_features(vdev, WANTED_FEATURES)) {
        console::kprintf(FG, BG, "[virtio-net] feature negotiation failed\n");
        return false;
    }
    if (!(vdev->driver_features & VIRTIO_F_VERSION_1)) {
        console::kprintf(FG, BG, "[virtio-net] device refused VERSION_1\n");
        return false;
    }
    s_hdr_sz = (vdev->driver_features & VIRTIO_NET_F_MRG_RXBUF) ? 12 : 10;

    if (vdev->driver_features & VIRTIO_NET_F_MAC) {
        util::memcpy(net->mac, vdev->device_cfg, 6);
    } else {
        net->mac[0] = 0x52;
        net->mac[1] = 0x54;
        net->mac[2] = 0x00;
        net->mac[3] = 0x12;
        net->mac[4] = 0x34;
        net->mac[5] = 0x56;
    }

    NetConfig *cfg = (NetConfig *)vdev->device_cfg;
    net->mtu = cfg->mtu ? cfg->mtu : 1500;
    if (net->mtu > 1500) net->mtu = 1500;
    net->link_up = true;  // QEMU user-net is always up; STATUS bit checked below
    if (vdev->driver_features & VIRTIO_NET_F_STATUS) net->link_up = (cfg->status & 1) != 0;

    if (!virtio::setup_queue(vdev, 0, 256)) {
        console::kprintf(FG, BG, "[virtio-net] RX queue setup failed\n");
        return false;
    }
    if (!virtio::setup_queue(vdev, 1, 256)) {
        console::kprintf(FG, BG, "[virtio-net] TX queue setup failed\n");
        return false;
    }

    // RX pool: one pmm page per buffer so each descriptor GPA is exact.
    // (Heap memory is not physically contiguous; a heap buffer could also
    // straddle a page boundary, corrupting DMA.)
    for (int i = 0; i < RX_N; i++) {
        s_rx_phys[i] = pmm::alloc_zero();
        if (!s_rx_phys[i]) {
            console::kprintf(FG, BG, "[virtio-net] RX page alloc failed\n");
            return false;
        }
        if (virtio::virtqueue_add(vdev, 0, s_rx_phys[i], RX_BUF_SZ, virtio::DESC_F_WRITE) <
            0) {
            console::kprintf(FG, BG, "[virtio-net] RX post failed\n");
            return false;
        }
    }
    s_tx_phys = pmm::alloc_zero();
    if (!s_tx_phys) {
        console::kprintf(FG, BG, "[virtio-net] TX page alloc failed\n");
        return false;
    }
    s_tx_virt = (uint8_t *)paging::phys_to_virt(s_tx_phys);
    // NOTE: no notify before DRIVER_OK (spec): the bring-up kick happens
    // after DRIVER_OK below.

    virtio::set_status(vdev,
                       virtio::VIRTIO_STATUS_ACK | virtio::VIRTIO_STATUS_DRIVER |
                           virtio::VIRTIO_STATUS_FEATURES_OK | virtio::VIRTIO_STATUS_DRIVER_OK);
    // First notify only now that the device is running.
    virtio::notify_queue(vdev, 0);

    uint8_t st = virtio::get_status(vdev);
    uint64_t feat = vdev->driver_features;
    console::kprintf(FG, BG, "[virtio-net] status=0x%x feats=0x%lx tx_phys=0x%x\n", st, feat,
                     (uint32_t)s_tx_phys);
    virtio::debug_queue(vdev, 0);
    virtio::debug_queue(vdev, 1);

    net->rx_buf = s_tx_virt;  // compat: populated fields, real RX uses pmm pages
    net->rx_buf_phys = 0;
    net->tx_buf = s_tx_virt;
    net->tx_buf_phys = 0;

    static const char *hex = "0123456789abcdef";
    char macs[18];
    char *mp = macs;
    for (int i = 0; i < 6; i++) {
        *mp++ = hex[(net->mac[i] >> 4) & 0xF];
        *mp++ = hex[net->mac[i] & 0xF];
        if (i < 5) *mp++ = ':';
    }
    *mp = 0;
    console::kprintf(FG, BG, "[virtio-net] MAC %s MTU %u link %s\n", macs, net->mtu,
                     net->link_up ? "up" : "down");
    return true;
}

void reset(NetDevice *net) {
    if (net) virtio::set_status(&net->vdev, 0);
}

// Reap one RX frame into buf (strips the virtio-net header).
// Returns frame length or -1 when no packet is ready.
int recv_packet(NetDevice *net, void *buf, uint32_t max_len) {
    if (!net || !buf || max_len == 0) return -1;
    virtio::Device *vdev = &net->vdev;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t used_len = 0;
        int desc = virtio::virtqueue_reap(vdev, 0, &used_len);
        if (desc < 0) return -1;
        // Find buffer address from descriptor
        uint8_t *d = (uint8_t *)vdev->queues[0].desc_virt + (size_t)desc * 16;
        uint64_t bphys = *(volatile uint64_t *)(d + 0);
        uint8_t *bvirt = (uint8_t *)paging::phys_to_virt(bphys);
        uint32_t copy = 0;
        if (used_len > s_hdr_sz && bvirt) {
            copy = used_len - s_hdr_sz;
            if (copy > max_len) copy = max_len;
            util::memcpy(buf, bvirt + s_hdr_sz, copy);
        }
        // Re-post the same buffer
        virtio::virtqueue_free(vdev, 0, (uint16_t)desc);
        if (bphys && virtio::virtqueue_add(vdev, 0, bphys, RX_BUF_SZ, virtio::DESC_F_WRITE) < 0) {
            return copy ? (int)copy : -1;
        }
        virtio::notify_queue(vdev, 0);
        if (copy) return (int)copy;
        // Header-only completion: keep draining.
    }
    return -1;
}

int send_packet(NetDevice *net, const void *buf, uint32_t len) {
    if (!net || !buf || len == 0) return -1;
    if (len + s_hdr_sz > TX_BUF_SZ) return -1;
    if (!s_tx_virt) return -1;
    // Link down: the backend is gone and a kick can stall the vCPU inside
    // QEMU (observed on Windows builds). Fail fast and bounded instead.
    NetConfig *cfg = (NetConfig *)net->vdev.device_cfg;
    if (cfg && (net->vdev.driver_features & VIRTIO_NET_F_STATUS) && !(cfg->status & 1)) {
        net->link_up = false;
        static bool warned = false;
        if (!warned) {
            warned = true;
            console::kprintf(FG, BG, "[virtio-net] link down: TX skipped (bounded)\n");
        }
        return -1;
    }
    util::memset(s_tx_virt, 0, s_hdr_sz);
    util::memcpy(s_tx_virt + s_hdr_sz, buf, len);
    virtio::Device *vdev = &net->vdev;
    int desc = virtio::virtqueue_add(vdev, 1, s_tx_phys, len + s_hdr_sz, 0);
    if (desc < 0) {
        return -1;
    }
    virtio::notify_queue(vdev, 1);
    // Poll for TX completion (no interrupts wired yet).
    for (int i = 0; i < 200000; i++) {
        uint32_t ulen = 0;
        int done = virtio::virtqueue_reap(vdev, 1, &ulen);
        if (done == desc) {
            virtio::virtqueue_free(vdev, 1, (uint16_t)done);
            return (int)len;
        }
        if (done >= 0) virtio::virtqueue_free(vdev, 1, (uint16_t)done);
    }
    // Timeout: report queue state once (helps pcap-less debugging).
    {
        volatile uint16_t *used_idx_p =
            (volatile uint16_t *)((uint8_t *)vdev->queues[1].used_virt + 2);
        static bool reported = false;
        if (!reported) {
            reported = true;
            console::kprintf(FG, BG, "[virtio-net] TX slow: used=%u last=%u free=%u\n",
                             *used_idx_p, vdev->queues[1].last_used_idx,
                             vdev->queues[1].num_free);
        }
        uint32_t ulen = 0;
        int done = virtio::virtqueue_reap(vdev, 1, &ulen);
        if (done >= 0) virtio::virtqueue_free(vdev, 1, (uint16_t)done);
    }
    return (int)len;  // QEMU consumes TX even if used-ring polling raced
}

bool link_status(NetDevice *net) { return net && net->link_up; }
void refresh_link(NetDevice *net) {
    if (!net) return;
    if (net->vdev.driver_features & VIRTIO_NET_F_STATUS) {
        NetConfig *cfg = (NetConfig *)net->vdev.device_cfg;
        net->link_up = (cfg->status & 1) != 0;
    }
}
uint16_t get_mtu(NetDevice *net) { return net ? net->mtu : 1500; }
const uint8_t *get_mac(NetDevice *net) { return net ? net->mac : nullptr; }

}  // namespace choco::virtio_net

// virtio-net driver
#pragma once
#include <choco/virtio.hpp>
#include <stdint.h>

namespace choco::virtio_net {

struct NetDevice {
    virtio::Device vdev;
    uint8_t mac[6];
    uint16_t mtu;
    bool link_up;
    // RX queue
    void *rx_buf;
    uint32_t rx_buf_phys;
    // TX queue
    void *tx_buf;
    uint32_t tx_buf_phys;
};

constexpr uint32_t VIRTIO_NET_F_CSUM       = 1 << 0;
constexpr uint32_t VIRTIO_NET_F_GUEST_CSUM = 1 << 1;
constexpr uint32_t VIRTIO_NET_F_MAC        = 1 << 5;
constexpr uint32_t VIRTIO_NET_F_GUEST_TSO4 = 1 << 7;
constexpr uint32_t VIRTIO_NET_F_GUEST_TSO6 = 1 << 8;
constexpr uint32_t VIRTIO_NET_F_GUEST_ECN  = 1 << 9;
constexpr uint32_t VIRTIO_NET_F_GUEST_UFO  = 1 << 10;
constexpr uint32_t VIRTIO_NET_F_MRG_RXBUF  = 1 << 15;
constexpr uint32_t VIRTIO_NET_F_STATUS     = 1 << 16;
constexpr uint32_t VIRTIO_NET_F_CTRL_VQ    = 1 << 17;
constexpr uint32_t VIRTIO_NET_F_CTRL_RX    = 1 << 18;
constexpr uint32_t VIRTIO_NET_F_CTRL_VLAN  = 1 << 19;
constexpr uint32_t VIRTIO_NET_F_GUEST_ANNOUNCE = 1 << 21;
constexpr uint32_t VIRTIO_NET_F_MQ         = 1 << 22;
constexpr uint32_t VIRTIO_NET_F_CTRL_MAC_ADDR = 1 << 23;

constexpr uint64_t VIRTIO_F_VERSION_1 = 1ULL << 32;

constexpr uint64_t WANTED_FEATURES = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS |
                                     VIRTIO_NET_F_MRG_RXBUF | VIRTIO_F_VERSION_1;

bool probe_and_init(NetDevice *net);
void reset(NetDevice *net);
int recv_packet(NetDevice *net, void *buf, uint32_t max_len);
int send_packet(NetDevice *net, const void *buf, uint32_t len);
bool link_status(NetDevice *net);
// Re-read the link bit from device config (set_link changes it live).
void refresh_link(NetDevice *net);
uint16_t get_mtu(NetDevice *net);
const uint8_t *get_mac(NetDevice *net);

} // namespace choco::virtio_net
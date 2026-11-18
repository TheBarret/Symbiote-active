#include <lib/mem.h>
#include <core/arch/heap.h>
#include <core/klog.h>
#include <core/services/router.h>

static network_device_t *device_head = NULL;

void router_init(void) {
    device_head = NULL;
    klog_serv("Router: in-memory message broker\n");
}

bool router_register_device(const char *name, uint32_t id, uint64_t address_base, size_t buffer_length, network_rx_callback_t handler) {
    // Check for duplicate ID
    for (network_device_t *d = device_head; d; d = d->next) {
        if (d->id == id) {
            klog_error("router: duplicate device ID 0x%x\n", id);
            return false;
        }
    }

    network_device_t *dev = (network_device_t *)kmalloc(sizeof(network_device_t));
    if (!dev) return false;

    dev->name = name;
    dev->id = id;
    dev->address_base = address_base;
    dev->buffer_length = buffer_length;

    // Allocate the device's internal buffer slice from your heap
    dev->buffer = (volatile uint8_t *)kmalloc(buffer_length);
    if (!dev->buffer) {
        kfree(dev);
        return false;
    }
    memset((void *)dev->buffer, 0, buffer_length);

    dev->rx_handler = handler;
    dev->next = device_head;
    device_head = dev;

    klog_serv("Routing '%s' on address=0x%x, base=0x%lx, length=%zu\n",
         name, id, address_base, buffer_length);
    return true;
}

// find a registered device by its destination address/ID
static network_device_t *find_device(uint32_t id) {
    for (network_device_t *d = device_head; d; d = d->next) {
        if (d->id == id) return d;
    }
    return NULL;
}

// send-and-forget datagram dispatch
bool router_send_packet(network_pframe *pkt) {
    if (!pkt) return false;

    network_device_t *recipient = find_device(pkt->dst_addr);
    if (!recipient) {
        // Drop packet (classic UDP behavior for unreachable destination)
        klog_warning("router: packet dropped (unknown dst 0x%x)\n", pkt->dst_addr);
        kfree(pkt);
        return false;
    }

    // If the recipient has a registered handler, deliver it
    if (recipient->rx_handler) {
        recipient->rx_handler(pkt);
    }

    // Fire-and-forget: clean up packet memory after delivery
    kfree(pkt);
    return true;
}

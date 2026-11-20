#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/router.h>
#include <core/services/vsink.h>
#include <core/services/io.h>
#include <core/services/rnd.h>
#include <core/services/greeter.h>

/* Number of multicast groups; bounded by the width of router_device.mcast_groups. */
#define ROUTER_MCAST_GROUPS 16u

static router_device *device_head = NULL;

/*  internal helpers  */

static router_device *find_device(router_address id) {
    for (router_device *d = device_head; d; d = d->next) {
        if (d->id == id) return d;
    }
    return NULL;
}

static inline const char *name_or_default(const char *name) {
    return name ? name : "<unnamed>";
}

static inline uint16_t mcast_bit(uint8_t group) {
    return (uint16_t)(1u << group);
}

/*  address matching  */

/* dispatch predicate */
bool router_isaddr(const router_device *dev, router_address dst) {
    if (!dev) return false;

    switch (addr_class(dst)) {
    case ADDR_CLASS_UNICAST:
        return dev->id == dst;

    case ADDR_CLASS_BROADCAST:
        return true;

    case ADDR_CLASS_MULTICAST: {
        uint16_t grp = addr_value(dst);
        if (grp >= ROUTER_MCAST_GROUPS) return false;
        return (dev->mcast_groups & mcast_bit((uint8_t)grp)) != 0;
    }

    case ADDR_CLASS_ANYCAST:
        klog_warning("router_isaddr() ADDR_CLASS_ANYCAST not implemented yet.\n");
        return false;

    default:
        return false;
    }
}

/*  device management - */

void register_sink(const char *name, router_address addr, router_rx_callback_t fn) {
    if (!router_register(name, addr, fn)) {
        klog_warning("router: failed to register sink '%s' at 0x%x\n",
                     name, (unsigned)addr);
    }
}

/* Note: `name` is stored by pointer, not copied; it must outlive the device. */
bool router_register(const char *name, router_address id, router_rx_callback_t handler) {
    if (!handler) return false;

    if (find_device(id) != NULL) {
        klog_warning("router: duplicate endpoint (address=0x%x, name=%s)\n",
                     (unsigned)id, name_or_default(name));
        return false;
    }

    router_device *dev = (router_device *)kmalloc(sizeof(router_device));
    if (!dev) return false;

    dev->name         = name;
    dev->id           = id;
    dev->mcast_groups = 0;
    dev->rx_handler   = handler;
    dev->next         = device_head;
    device_head       = dev;

    klog_serv("Endpoint %s created at address=0x%x\n",
              name_or_default(name), (unsigned)id);
    return true;
}

/*
#define GROUP_LOGGERS 5   // any group 0..15

static void mcast_test(void) {
    // devices must be registered first, otherwise join returns false
    //router_register("print",  ADDR_UNICAST(1), route_to_console);
    //router_register("printv", ADDR_UNICAST(2), route_to_consolev);

    // subscribe both to group 5
    router_join_mcast(ADDR_UNICAST(1), GROUP_LOGGERS);
    router_join_mcast(ADDR_UNICAST(2), GROUP_LOGGERS);

    // one frame to the group: both sinks receive it
    const char msg[] = "Hi";
    router_send(router_create(ADDR_NOBODY, ADDR_MULTICAST(GROUP_LOGGERS),
                              FRAME_DATA, (const uint8_t *)msg, sizeof(msg) - 1u));
    // unsubscribe printv: only print receives the next one
    router_leave_mcast(ADDR_UNICAST(2), GROUP_LOGGERS);
    router_send(router_create(ADDR_NOBODY, ADDR_MULTICAST(GROUP_LOGGERS),
                              FRAME_DATA, (const uint8_t *)msg, sizeof(msg) - 1u));
}
*/

bool router_join_mcast(router_address id, uint8_t group) {
    if (group >= ROUTER_MCAST_GROUPS) return false;

    router_device *dev = find_device(id);
    if (!dev) return false;

    dev->mcast_groups |= mcast_bit(group);
    return true;
}

bool router_leave_mcast(router_address id, uint8_t group) {
    if (group >= ROUTER_MCAST_GROUPS) return false;

    router_device *dev = find_device(id);
    if (!dev) return false;

    dev->mcast_groups &= (uint16_t)~mcast_bit(group);
    return true;
}

/*  transport - */

/* Takes ownership of pkt and always frees it, whether or not it was delivered. */
bool router_send(router_frame *pkt) {
    if (!pkt) return false;

    bool delivered = false;

    if (addr_is_unicast(pkt->dst_addr)) {
        /* fast path: exact match, single dispatch. */
        router_device *dev = find_device(pkt->dst_addr);
        if (dev) {
            dev->rx_handler(pkt);
            delivered = true;
        }
    } else if (addr_is_anycast(pkt->dst_addr)) {
        /* warn once per frame instead of once per registered device. */
        klog_warning("router_send() ADDR_CLASS_ANYCAST not implemented yet.\n");
    } else {
        /* cast path: dispatch to every matching device. */
        for (router_device *d = device_head; d; d = d->next) {
            if (router_isaddr(d, pkt->dst_addr)) {
                d->rx_handler(pkt);     /* handler must not retain pkt */
                delivered = true;
            }
        }
    }

    kfree(pkt);
    return delivered;
}

router_frame *router_create(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype,
                            const uint8_t *payload, uint16_t payload_len) {
    if (payload_len > 0 && !payload) return NULL;

    /* guard against unsigned underflow if the limit were ever <= header size */
    if (ROUTER_SIZE_MAX <= sizeof(router_frame)) return NULL;
    if (payload_len > ROUTER_SIZE_MAX - sizeof(router_frame)) return NULL;

    size_t total = sizeof(router_frame) + (size_t)payload_len;
    router_frame *frame = (router_frame *)kmalloc(total);
    if (!frame) return NULL;

    frame->src_addr = src_addr;
    frame->dst_addr = dst_addr;
    frame->type     = ptype;
    frame->length   = payload_len;
    if (payload_len > 0) {
        memcpy(frame->payload, payload, payload_len);
    }
    return frame;
}

/*  selftest / init  */

/* Broadcast test
 * Exercises the cast path via ADDR_BROADCAST. Passes only if at least one
 * endpoint is registered. The anycast path stays untested until implemented.
 */
void router_perform_test(void) {
    klog_serv("Router: running broadcast test");

    router_frame *bc = router_create(ADDR_UNICAST(99), ADDR_BROADCAST,
                                     FRAME_EMPTY, NULL, 0);
    kprintf(router_send(bc) ? "...[OK]\n" : "...[FAILED]\n");
}

/* init: drops any previously registered devices so re-init does not leak. */
void router_init(void) {
    klog_serv("Setting up environment...\n");
    router_device *d = device_head;
    while (d) {
        router_device *next = d->next;
        kfree(d);
        d = next;
    }
    device_head = NULL;
}

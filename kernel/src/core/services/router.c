#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/decoder.h>
#include <core/services/router.h>

/* Router concurrency contract:
 * the router is NOT interrupt-safe. The device list and the heap are unlocked,
 * so router_send() and friends must only be called from task context.
 * Interrupt handlers should queue work and let a loop task send (the way timer_drain() does).
 */

/* largest payload a frame may carry */
#define ROUTER_MAX_PAYLOAD ((size_t)ROUTER_SIZE_MAX - sizeof(router_frame))

_Static_assert((size_t)ROUTER_SIZE_MAX > sizeof(router_frame),
               "router: ROUTER_SIZE_MAX too small for the frame header");
_Static_assert(ROUTER_MAX_PAYLOAD <= UINT16_MAX,
               "router: max payload must fit router_frame.length (uint16_t)");
_Static_assert(ROUTER_MCAST_GROUPS <= 16u,
               "router: ROUTER_MCAST_GROUPS exceeds router_device.mcast_groups width");

static router_device *device_head = NULL;
static uint8_t        g_depth     = 0;     /* current router_send() nesting */
static uint32_t       g_connected = 0;     /* accumulates on each router_register() */
static uint64_t       g_success   = 0;     /* on success tracking */
static uint64_t       g_rejects   = 0;     /* on reject tracking */
static uint64_t       g_voided    = 0;     /* void packets tracking */

/*  internal helpers  */

static void router_reset_metrics(void) {
    g_success   = 0;
    g_rejects   = 0;
    g_voided    = 0;
    g_connected = 0;
}

static router_device *find_device(router_address id) {
    for (router_device *d = device_head; d; d = d->next) {
        if (d->id == id) return d;
    }
    return NULL;
}

/* anycast: first match in list order, i.e. the most recently registered provider */
static router_device *router_resolve_anycast(uint16_t service_id) {
    if (service_id == 0) return NULL;

    for (router_device *d = device_head; d; d = d->next) {
        if (d->service_id == service_id) return d;
    }
    return NULL;
}

static inline const char *name_or_default(const char *name) {
    return name ? name : "anonymous"; /* undefined resolves as anonymous */
}

static inline uint16_t mcast_bit(uint8_t group) {
    return (uint16_t)(1u << group);
}

/* Frame validation, reject frames with conflicting properties.
 * pkt is non-NULL: router_send() checks before calling.
 */
static bool frame_is_sane(const router_frame *pkt) {
    if ((size_t)pkt->length > ROUTER_MAX_PAYLOAD) {
        klog_warning("illegal frame (length %u exceeds max payload %zu)\n",
                     (unsigned)pkt->length, (size_t)ROUTER_MAX_PAYLOAD);
        return false;
    }
    if (pkt->type > FRAME_STRUCT) {
        klog_warning("illegal frame (invalid type %u)\n", (unsigned)pkt->type);
        return false;
    }
    /* protected address space is not a valid destination */
    if (addr_is_protected(pkt->dst_addr)) {
        klog_warning("illegal frame (protected dst class %u)\n",
                     (unsigned)addr_class(pkt->dst_addr));
        return false;
    }
    if (addr_is_multicast(pkt->dst_addr) && addr_value(pkt->dst_addr) >= ROUTER_MCAST_GROUPS) {
        klog_warning("illegal frame (multicast group %u out of range)\n",
                     (unsigned)addr_value(pkt->dst_addr));
        return false;
    }
    return true;
}

/* frame inspection */
void router_inspect_frame(const router_frame *pkt) {
    kprintf("src(%u) → dst(%u)\n", (unsigned)pkt->src_addr, (unsigned)pkt->dst_addr);
    kprintf("type      : %u, port     : %u\n", (unsigned)pkt->type, (unsigned)pkt->port);
    kprintf("length    : %u, sentinel : %llu\n", (unsigned)pkt->length,
            (unsigned long long)pkt->sentinel);
    if (pkt->type == FRAME_STRUCT) {
        kprintf("Decoder   : %s\n",
                frame_check(pkt->payload, pkt->length) == DECODER_OK ? "accepted" : "rejected");
    }
}

/* in-line broadcast tester */
static bool router_iftest(void) {
    return router_send(router_create_p(ADDR_NOBODY, ADDR_BROADCAST, FRAME_SIGNAL, 0, NULL, 0));
}

/* general stats */
void router_stats(void) {
    kprintf("Router status\n");
    kprintf("status       : %s\n", router_iftest() ? "accepted" : "rejected");
    kprintf("connected    : %u\n",   (unsigned)g_connected);
    kprintf("success      : %llu\n", (unsigned long long)g_success);
    kprintf("nulled       : %llu\n", (unsigned long long)g_voided);
    kprintf("rejects      : %llu\n", (unsigned long long)g_rejects);
    kprintf("depth        : %u\n",   (unsigned)g_depth);
}

/* anycast service binding */
bool router_bind_service(router_address id, uint16_t service_id) {
    if (service_id == 0 || service_id > ADDR_VALUE_MASK) return false;

    router_device *dev = find_device(id);
    if (!dev) return false;

    dev->service_id = service_id;
    return true;
}

/* address matching, dispatch predicate */
bool router_isaddr(const router_device *dev, router_address dst) {
    if (!dev) return false;

    switch (addr_class(dst)) {
    case ADDR_CLASS_UNICAST:
        return dev->id == dst;

    case ADDR_CLASS_BROADCAST:
        return true;

    case ADDR_CLASS_MULTICAST: {
        const uint16_t grp = addr_value(dst);
        return grp < ROUTER_MCAST_GROUPS && (dev->mcast_groups & mcast_bit((uint8_t)grp)) != 0;
    }
    case ADDR_CLASS_ANYCAST: {
        const uint16_t svc = addr_value(dst);
        return svc != 0 && dev->service_id == svc;
    }
    default:
        return false;
    }
}

/* device management
 * `name` is stored by pointer, not copied; it must outlive the device.
 * `id` must be a unicast address: cast classes are destinations, not identities.
 * Safe to call from inside a handler:
 * new devices are prepended, so a dispatch already in progress is not disturbed
 * (the new device just misses that frame).
 */
bool router_register(const char *name, router_address id, router_rx_callback_t handler) {
    if (!handler) return false;

    if (!addr_is_unicast(id)) {
        klog_warning("router: endpoint must be unicast (address=0x%x, name=%s)\n",
                     (unsigned)id, name_or_default(name));
        return false;
    }

    if (find_device(id) != NULL) {
        klog_warning("router: duplicate endpoint (address=0x%x, name=%s)\n",
                     (unsigned)id, name_or_default(name));
        return false;
    }

    router_device *dev = (router_device *)kmalloc(sizeof(router_device));
    if (!dev) return false;

    g_connected++; /* accumulate metric */
    dev->name         = name_or_default(name);
    dev->id           = id;
    dev->mcast_groups = 0;
    dev->service_id   = 0;
    dev->rx_handler   = handler;
    dev->next         = device_head;
    dev->index        = g_connected;
    device_head       = dev;
    klog_serv("endpoint %s at 0x%04x\n", dev->name, (unsigned)id);
    return true;
}

void register_sink(const char *name, router_address addr, router_rx_callback_t fn) {
    if (!router_register(name, addr, fn)) {
        klog_warning("router: failed to register sink '%s' at 0x%x\n",
                     name_or_default(name), (unsigned)addr);
    }
}

bool router_hotwire(router_address id, router_rx_callback_t handler) {
    return router_register(NULL, id, handler);
}

/* multicast example:
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

/* multicast group operations */
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

/* Dispatch pkt to its destination(s), returns true if at least one handler ran. */
static bool router_dispatch(router_frame *pkt) {
    const router_address dst = pkt->dst_addr;
    router_device *dev;
    if (addr_is_unicast(dst)) {
        /* fast path: exact match, single dispatch */
        dev = find_device(dst);
    } else if (addr_is_anycast(dst)) {
        dev = router_resolve_anycast(addr_value(dst));
    } else if (addr_is_protected(dst)) {
        /* todo: reserved for interior bus traffic */
        klog_warning("router_send() frame dropped (ADDR_CLASS_PROTECTED)\n");
        router_inspect_frame(pkt);
        return false;
    } else {
        /* cast path: dispatch to every matching device */
        bool delivered = false;
        for (router_device *d = device_head; d; d = d->next) {
            if (router_isaddr(d, dst)) {
                d->rx_handler(pkt);
                delivered = true;
            }
        }
        return delivered;
    }

    if (!dev) return false;
    dev->rx_handler(pkt);
    return true;
}

/* Drop a frame: count the reject, log why, free it. */
static bool router_drop(router_frame *pkt, const char *why) {
    g_rejects++;
    klog_warning("router dropped frame (%s)\n", why);
    router_inspect_frame(pkt);
    kfree(pkt);
    return false;
}

/* Send transport
 * Takes ownership of pkt and frees it exactly once on every path, whether or
 * not it was delivered. Handlers may send further frames (replies); nesting is
 * capped at ROUTER_MAX_DEPTH. Handlers must not retain, free or modify pkt:
 * on a cast, every matching device receives the same frame.
 */
bool router_send(router_frame *pkt) {
    if (!pkt) return false;

    if (!frame_is_sane(pkt))          return router_drop(pkt, "frame_is_sane=false");
    if (g_depth >= ROUTER_MAX_DEPTH)  return router_drop(pkt, "g_depth >= ROUTER_MAX_DEPTH");

    g_depth++;
    const bool delivered = router_dispatch(pkt);
    g_depth--;

    if (delivered) g_success++;
    else           g_rejects++;

    kfree(pkt);
    return delivered;
}

/* create packet with port descriptor */
router_frame *router_create_p(router_address src_addr, router_address dst_addr,
                              router_ptypes ptype, uint16_t port,
                              const uint8_t *payload, uint16_t payload_len) {
    if (payload_len > 0 && !payload) return NULL;
    if (payload_len > ROUTER_MAX_PAYLOAD) return NULL;

    router_frame *frame = (router_frame *)kmalloc(sizeof(router_frame) + payload_len);
    if (!frame) return NULL;

    frame->sentinel = g_success;
    frame->src_addr = src_addr;
    frame->dst_addr = dst_addr;
    frame->type     = (uint8_t)ptype;
    frame->port     = port;
    frame->length   = payload_len;
    if (payload_len > 0) memcpy(frame->payload, payload, payload_len);
    return frame;
}

/* create packet without port descriptor, legacy */
router_frame *router_create(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype,
                            const uint8_t *payload, uint16_t payload_len) {
    return router_create_p(src_addr, dst_addr, ptype, 0 /* 0 = default */, payload, payload_len);
}

/* built-in void sink */
static void route_to_void(router_frame *pkt) { (void)pkt; g_voided++; }
static void init_vsink(void) { register_sink("void", ADDR_NOBODY, route_to_void); }

/* Broadcast test
 * Exercises the cast path via ADDR_BROADCAST, passes only if at least one endpoint is registered.
 */
bool router_perform_test(router_address id) {
    klog_serv("sending broadcast");
    router_frame *bc = router_create_p(id, ADDR_BROADCAST, FRAME_SIGNAL, 0, NULL, 0);
    bool r_val = router_send(bc);
    kprintf(r_val ? "...[OK]\n" : "...[FAILED]\n");
    return r_val;
}

/* initializer: frees every device, refuses while a dispatch is in progress,
 * because the dispatch loop is walking the list being freed.
 */
void router_init(void) {
    if (g_depth > 0) return;
    klog_serv("Setting up environment...\n");
    router_device *d = device_head;
    while (d) {
        router_device *next = d->next;
        kfree(d);
        d = next;
    }
    device_head = NULL;
    router_reset_metrics();
    init_vsink();
    //router_perform_test(ADDR_NOBODY);
}

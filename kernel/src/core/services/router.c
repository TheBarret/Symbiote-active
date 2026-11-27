#include <lib/mem.h>
#include <lib/string.h>
#include <lib/random.h>
#include <core/version.h>
#include <core/arch/heap.h>
#include <core/dev/vfs.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/decoder.h>
#include <core/services/router.h>
#include <core/services/logger.h>

/* Concurrency contract: the router is NOT interrupt-safe. The device list and
 * the heap are unlocked, so router_send() and friends run in task context only.
 * Interrupt handlers queue work and let a loop task send (as timer_drain() does).
 */

_Static_assert(ROUTER_MCAST_GROUPS <= 16u,
               "router: ROUTER_MCAST_GROUPS exceeds router_device.mcast_groups width");

static router_device *device_head = NULL;
static uint8_t        g_depth     = 0;     /* current router_send() nesting */
static uint32_t       g_connected = 0;     /* accumulates on each router_register() */
static uint64_t       g_success   = 0;
static uint64_t       g_rejects   = 0;
static uint64_t       g_voided    = 0;

/* internal helpers */

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

/* anycast: first match in list order, the most recently registered provider */
static router_device *router_resolve_anycast(uint16_t service_id) {
    if (service_id == 0) return NULL;

    for (router_device *d = device_head; d; d = d->next) {
        if (d->service_id == service_id) return d;
    }
    return NULL;
}

static inline const char *name_or_default(const char *name) {
    return name ? name : "anonymous";
}

static inline uint16_t mcast_bit(uint8_t group) {
    return (uint16_t)(1u << group);
}

/* Reject frames with conflicting properties. pkt is non-NULL (checked by router_send). */
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
    /* only unicast addresses are identities */
    if (!addr_is_unicast(pkt->src_addr)) {
        klog_warning("illegal frame (non-unicast src class %u)\n",
                     (unsigned)addr_class(pkt->src_addr));
        return false;
    }
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

void router_inspect_frame(const router_frame *pkt) {
    kprintf("src(%u) → dst(%u)\n", (unsigned)pkt->src_addr, (unsigned)pkt->dst_addr);
    kprintf("type      : %u, port     : %u\n", (unsigned)pkt->type, (unsigned)pkt->port);
    kprintf("length    : %u, sentinel : %u\n", (unsigned)pkt->length, (unsigned)pkt->sentinel);
    if (pkt->type == FRAME_STRUCT) {
        kprintf("Decoder   : %s\n",
                frame_check(pkt->payload, pkt->length) == DECODER_OK ? "accepted" : "rejected");
    }
}

/* in-line broadcast tester */
bool router_iftest(void) {
    return router_send(router_create_p(ADDR_UNICAST(NOBODY), ADDR_BROADCAST,
                                       FRAME_SIGNAL, DEFAULT_BC, NULL, 0));
}

void router_stats(void) {
    kprintf("Router status\n");
    kprintf("status       : %s\n", router_iftest() ? "accepted" : "rejected");
    kprintf("connected    : %u\n",   (unsigned)g_connected);
    kprintf("success      : %llu\n", (unsigned long long)g_success);
    kprintf("nulled       : %llu\n", (unsigned long long)g_voided);
    kprintf("rejects      : %llu\n", (unsigned long long)g_rejects);
    kprintf("depth        : %u\n",   (unsigned)g_depth);
}

bool router_bind_service(router_address id, uint16_t service_id) {
    if (service_id == 0 || service_id > ADDR_VALUE_MASK) return false;

    router_device *dev = find_device(id);
    if (!dev) return false;

    dev->service_id = service_id;
    return true;
}

/* dispatch predicate: does dev match dst? */
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

/* `name` is stored by pointer and must outlive the device.
 * `id` must be unicast: cast classes are destinations, not identities.
 * Safe to call from inside a handler: new devices are prepended, so a dispatch
 * in progress is undisturbed (the new device just misses that frame).
 */
bool router_register(const char *name, router_address id, router_rx_callback_t handler) {
    if (!handler) return false;

    if (!addr_is_unicast(id)) {
        log_write("    router_register() endpoint must be unicast (address=0x%04x, name=%s)\n",
                  (unsigned)id, name_or_default(name));
        return false;
    }

    if (find_device(id) != NULL) {
        log_write("    router_register() duplicate endpoint %s at 0x%04x\n",
                  name_or_default(name), (unsigned)id);
        return false;
    }

    router_device *dev = (router_device *)kmalloc(sizeof(router_device));
    if (!dev) return false;

    g_connected++;

    dev->name         = name_or_default(name);
    dev->id           = id;
    dev->mcast_groups = 0;
    dev->service_id   = 0;
    dev->rx_handler   = handler;
    dev->next         = device_head;
    dev->index        = g_connected;
    device_head       = dev;

    klog_serv("endpoint %s at 0x%04x\n", dev->name, (unsigned)id);
    log_write("    router_register() endpoint %s at 0x%04x\n", dev->name, (unsigned)id);
    return true;
}

void register_sink(const char *name, router_address addr, router_rx_callback_t fn) {
    if (!router_register(name, addr, fn)) {
        log_write("    register_sink() endpoint %s at 0x%04x, failed\n",
                  name_or_default(name), (unsigned)addr);
    }
}

bool router_hotwire(router_address id, router_rx_callback_t handler) {
    return router_register(NULL, id, handler);
}

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

/* Dispatch pkt to its destination(s). True if at least one handler ran. */
static bool router_dispatch(router_frame *pkt) {
    const router_address dst = pkt->dst_addr;
    const unsigned sid = pkt->sentinel;
    router_device *dev;

    log_write("       [%u] dst = 0x%04x\n", sid, (unsigned)dst);
    log_write("       [%u] src = 0x%04x\n", sid, (unsigned)pkt->src_addr);

    if (addr_is_unicast(dst)) {
        dev = find_device(dst);
        log_write("       [%u] type = unicast\n", sid);
    } else if (addr_is_anycast(dst)) {
        dev = router_resolve_anycast(addr_value(dst));
        log_write("       [%u] type = anycast\n", sid);
    } else if (addr_is_protected(dst)) {
        /* unreachable: frame_is_sane() rejects it first. Kept as defense in depth. */
        log_write("       [%u] type = protected\n", sid);
        return false;
    } else {
        unsigned n = 0;
        log_write("       [%u] type = %s\n", sid,
                  addr_is_multicast(dst) ? "multicast" : "broadcast");
        for (router_device *d = device_head; d; d = d->next) {
            if (router_isaddr(d, dst)) {
                d->rx_handler(pkt);
                n++;
            }
        }
        if (n) log_write("       [%u] status = delivered (%u)\n", sid, n);
        else   log_write("       [%u] status = undeliverable\n", sid);
        return n != 0;
    }

    if (!dev) {
        log_write("       [%u] status = undeliverable\n", sid);
        return false;
    }
    dev->rx_handler(pkt);
    log_write("       [%u] status = delivered\n", sid);
    return true;
}

/* Drop a frame: count the reject, log why, free it. */
static bool router_drop(router_frame *pkt, const char *why) {
    g_rejects++;
    log_write("    router_drop(%u) dropped (%s)\n", (unsigned)pkt->sentinel, why);
    kfree(pkt);
    return false;
}

/* Takes ownership of pkt and frees it exactly once on every path.
 * Handlers may send further frames (replies); nesting is capped at
 * ROUTER_MAX_DEPTH. Handlers must not retain, free or modify pkt: on a cast,
 * every matching device receives the same frame.
 */
bool router_send(router_frame *pkt) {
    if (!pkt) return false;
    if (!frame_is_sane(pkt))         return router_drop(pkt, "frame_is_sane=false");
    if (g_depth >= ROUTER_MAX_DEPTH) return router_drop(pkt, "g_depth >= ROUTER_MAX_DEPTH");

    const unsigned sid = pkt->sentinel;   /* read before the frame is freed */

    g_depth++;
    log_write("    router_send(depth=%u,%u)\n", (unsigned)g_depth, sid);
    const bool delivered = router_dispatch(pkt);
    g_depth--;

    if (delivered) g_success++;
    else           g_rejects++;

    kfree(pkt);
    log_write("    router_send(depth=%u)->kfree(%u)\n", (unsigned)g_depth, sid);
    return delivered;
}

router_frame *router_create_p(router_address src_addr, router_address dst_addr,
                              router_ptypes ptype, uint16_t port,
                              const uint8_t *payload, uint16_t payload_len) {
    if (payload_len > 0 && !payload) return NULL;
    if (payload_len > ROUTER_MAX_PAYLOAD) return NULL;

    router_frame *frame = (router_frame *)kmalloc(sizeof(router_frame) + payload_len);
    if (!frame) return NULL;

    const uint16_t sid = (uint16_t)rnd_next();
    frame->sentinel = sid;
    frame->src_addr = src_addr;
    frame->dst_addr = dst_addr;
    frame->type     = (uint8_t)ptype;
    frame->port     = port;
    frame->length   = payload_len;
    if (payload_len > 0) memcpy(frame->payload, payload, payload_len);

    log_write("    router_create_p(%u,port=%u)->kmalloc(payload=%u)\n",
              (unsigned)sid, (unsigned)port, (unsigned)payload_len);
    return frame;
}

/* no port descriptor (port 0) */
router_frame *router_create(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype,
                            const uint8_t *payload, uint16_t payload_len) {
    return router_create_p(src_addr, dst_addr, ptype, 0, payload, payload_len);
}

/* void: the anonymous sink */
static void route_to_void(router_frame *pkt) { (void)pkt; g_voided++; }
static void init_vsink(void) { register_sink("void", ADDR_UNICAST(NOBODY), route_to_void); }

/* Broadcast test: passes only if at least one endpoint is registered. */
bool router_perform_test(router_address id) {
    klog_serv("sending broadcast");
    router_frame *bc = router_create_p(id, ADDR_BROADCAST, FRAME_SIGNAL, DEFAULT_BC, NULL, 0);
    bool r_val = router_send(bc);
    kprintf(r_val ? "...[OK]\n" : "...[FAILED]\n");
    return r_val;
}

/* Frees every device. Refuses while a dispatch is in progress, because the
 * dispatch loop would be walking the list being freed.
 */
void router_init(void) {
    if (g_depth > 0) {
        log_write("    router_init() refused, dispatch in progress (depth=%u)\n", (unsigned)g_depth);
        return;
    }
    log_write("    router_init()\n");
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
}

#ifndef CORE_SERVICES_ROUTER_H
#define CORE_SERVICES_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/services/addr.h>
/* codebase: 0.2.0 */

/* Router: in-memory message broker
 *
 * [ Router ]
 *     |-------[router_device: addr unicast @ &call_route_to_void()]
 *     |-------[router_device: addr unicast @ &route_to_console()]
 *     |-------[router_device: addr unicast @ &route_to_consolev()]
 *     |
 *    API: [router_register]
 *         [router_send]
 *         [router_join_mcast]
 *         [router_leave_mcast]
 *         [router_create]
 *         [router_isaddr]
 *
 * Transport:
 * [router_device A] -> [router_frame: src, dst, type, length, payload ] -> [router_device B]
 *
 * changelog rev: 0.2
 * - Depth guard:
 *  `router_send` now counts its nesting and drops the frame once it reaches `ROUTER_MAX_DEPTH`,
 *   which is 8 unless you override it in `config.h`.
 *   A reply cycle like A→B→A is cut off with a warning, and the frame is still freed.
 * - Frame validation:
 *   a handler trusts `pkt->length`, so `router_send` now drops any frame with an out-of-range length or an invalid type before dispatch.
 *   Frames from `router_create` always pass. The check only catches frames built by hand.
 * - Unicast-only registration:
 *   `router_register` now rejects any `id` that isn't a unicast address. Before,
 *   a device could register with a broadcast or multicast address as its identity.
 * - Safe re-init:
 *   `router_init` refuses to run while a dispatch is in progress, because it would free the device list the dispatch loop is walking.
 * - Reserved classes:
 *   frames addressed to a reserved class return early instead of scanning every device.
 * - NULL name bug:
 *   `register_sink` passed a possibly-NULL name straight to the log format. It now uses `name_or_default`.
 *
 */

/* transport class */
typedef enum {
    FRAME_SIGNAL      = 0, /* signal */
    FRAME_DATA        = 1, /* raw */
    FRAME_ENVELOPE    = 2, /* structured */
    FRAME_EMPTY       = 3, /* debug */
} router_ptypes;

/* transport packet frame */
typedef struct {
    uint64_t        sentinel; /* router->g_success accumulator, unique, wraps */
    router_address  src_addr; /* source address */
    router_address  dst_addr; /* destination address */
    router_ptypes       type; /* transport class */
    uint16_t            port; /* local port, 0 is default */
    uint16_t          length; /* bytes in payload */
    uint8_t        payload[]; /* buffer array member */
} __attribute__((packed)) router_frame;

/* transport callback signature: frame is owned by the router, must not be retained */
typedef void (*router_rx_callback_t)(router_frame *pkt);

typedef struct router_device {
    const char                *name; /* string name */
    router_address               id; /* unicast address, assigned at register */
    uint16_t           mcast_groups; /* bit N = member of group N (0..15) */
    uint16_t             service_id; /* anycast service ID, 0 = none, 1..0xFFF service descriptor */
    router_rx_callback_t rx_handler; /* called on dispatch */
    struct router_device      *next; /* list linkage */
} router_device;

/* transport router API */
void router_init(void);
bool router_register(const char *name, router_address id, router_rx_callback_t handler);
bool router_hotwire(router_address id, router_rx_callback_t handler);
void register_sink(const char *name, router_address addr, router_rx_callback_t fn);
bool router_join_mcast(router_address id, uint8_t group);
bool router_leave_mcast(router_address id, uint8_t group);

/* transport send: router takes ownership and frees it after dispatch, returns true if at least one handler was invoked. */
bool router_send(router_frame *pkt);

/* transport build: caller owns until passed to router_send. */
/* legacy */
router_frame *router_create(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype, const uint8_t *payload, uint16_t payload_len);
/* with port */
router_frame *router_create_p(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype, uint16_t port,
                            const uint8_t *payload, uint16_t payload_len);

/* transport anycast service binder */
bool router_bind_service(router_address id, uint16_t service_id);

/* transport address predicate helper */
bool router_isaddr(const router_device *dev, router_address dst);

/* selftesters */
void router_perform_test(router_address id);

/* stats */
void router_stats(void);

#endif /* CORE_SERVICES_ROUTER_H */

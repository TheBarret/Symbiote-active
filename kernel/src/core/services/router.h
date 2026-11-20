#ifndef CORE_SERVICES_ROUTER_H
#define CORE_SERVICES_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/services/addr.h>

/* Router: in-memory message broker
 *
 * [ Router ]
 *     |-------[router_device: addr unicast 0 @ &call_route_to_void()]
 *     |-------[router_device: addr unicast 1 @ &route_to_console()]
 *     |-------[router_device: addr unicast 2 @ &route_to_consolev()]
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
 *
 */

/* transport class */
typedef enum {
    FRAME_EMPTY  = 0,
    FRAME_DATA   = 1,
    FRAME_SIGNAL = 2
} router_ptypes;

/* transport packet frame */
typedef struct {
    router_address src_addr;   /* source address */
    router_address dst_addr;   /* destination address */
    router_ptypes  type;       /* transport class */
    uint16_t       length;     /* bytes in payload */
    uint8_t        payload[];  /* flexible array member */
} __attribute__((packed)) router_frame;

/* transport callback signature: frame is owned by the router, must not be retained */
typedef void (*router_rx_callback_t)(router_frame *pkt);

typedef struct router_device {
    const char          *name;         /* string name */
    router_address         id;           /* unicast address, assigned at register */
    uint16_t             mcast_groups; /* bit N = member of group N (0..15) */
    router_rx_callback_t rx_handler;   /* called on dispatch */
    struct router_device *next;        /* list linkage */
} router_device;

/* transport router API */
void router_init(void);
void init_vsink(router_address id);
bool router_register(const char *name, router_address id, router_rx_callback_t handler);
void register_sink(const char *name, router_address addr, router_rx_callback_t fn);
bool router_join_mcast(router_address id, uint8_t group);
bool router_leave_mcast(router_address id, uint8_t group);

/* transport send: router takes ownership and frees it after dispatch, returns true if at least one handler was invoked. */
bool router_send(router_frame *pkt);

/* transport build: caller owns until passed to router_send. */
router_frame *router_create(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype,
                            const uint8_t *payload, uint16_t payload_len);

/* transport address predicate helper */
bool router_isaddr(const router_device *dev, router_address dst);

/* broadcast selftest */
void router_perform_test(void);

#endif /* CORE_SERVICES_ROUTER_H */

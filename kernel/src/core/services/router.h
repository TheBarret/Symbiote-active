#ifndef CORE_SERVICES_ROUTER_H
#define CORE_SERVICES_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <core/services/addr.h>
/* codebase: 0.2.0 */

/* Router: in-memory message broker.
 * Devices register a unicast address and an rx handler. router_send() takes
 * ownership of a frame, validates it, dispatches it and frees it exactly once.
 * Not interrupt-safe: send from task context only.
 */

/* transport class */
typedef enum {
    FRAME_SIGNAL      = 0, /* control: signal frames, payload may carry a VM image */
    FRAME_DATA        = 1, /* transport: data, unstructured */
    FRAME_STRUCT      = 2  /* transport: data, structured (see decoder.h) */
} router_ptypes;

/* transport packet frame */
typedef struct {
    uint16_t        sentinel; /* random id, for log correlation only */
    router_address  src_addr; /* source, must be unicast */
    router_address  dst_addr; /* destination */
    uint8_t             type; /* a router_ptypes value */
    uint16_t            port; /* local port, 0 = broadcast default */
    uint16_t          length; /* bytes in payload */
    uint8_t        payload[]; /* payload bytes, unaligned (offset 11): read bytewise or memcpy */
} __attribute__((packed)) router_frame;

/* ROUTER_SIZE_MAX is the whole frame, header included */
#define ROUTER_HEADER_SIZE  sizeof(router_frame)
#define ROUTER_MAX_PAYLOAD  ((size_t)ROUTER_SIZE_MAX - ROUTER_HEADER_SIZE)

_Static_assert(offsetof(router_frame, payload) == 11, "router_frame layout changed");
_Static_assert(sizeof(router_frame) == 11,            "router_frame must be a bare header");
_Static_assert((size_t)ROUTER_SIZE_MAX > sizeof(router_frame),
               "router: ROUTER_SIZE_MAX too small for the frame header");
_Static_assert(ROUTER_MAX_PAYLOAD <= UINT16_MAX,
               "router: max payload must fit router_frame.length (uint16_t)");

/* rx callback: the frame is owned by the router, must not be retained, freed or modified */
typedef void (*router_rx_callback_t)(router_frame *pkt);

typedef struct router_device {
    const char                *name; /* stored by pointer, must outlive the device */
    router_address               id; /* unicast address */
    uint16_t           mcast_groups; /* bit N = member of group N (0..15) */
    uint16_t             service_id; /* anycast service id, 0 = none */
    router_rx_callback_t rx_handler;
    struct router_device      *next;
    uint32_t                  index; /* registration order */
} router_device;

void router_init(void);
bool router_register(const char *name, router_address id, router_rx_callback_t handler);
bool router_hotwire(router_address id, router_rx_callback_t handler);
void register_sink(const char *name, router_address addr, router_rx_callback_t fn);
bool router_join_mcast(router_address id, uint8_t group);
bool router_leave_mcast(router_address id, uint8_t group);
bool router_bind_service(router_address id, uint16_t service_id);
bool router_isaddr(const router_device *dev, router_address dst);

/* takes ownership of pkt; true if at least one handler ran */
bool router_send(router_frame *pkt);

/* caller owns the frame until it is passed to router_send */
router_frame *router_create(router_address src_addr, router_address dst_addr,
                            router_ptypes ptype, const uint8_t *payload, uint16_t payload_len);
router_frame *router_create_p(router_address src_addr, router_address dst_addr,
                              router_ptypes ptype, uint16_t port,
                              const uint8_t *payload, uint16_t payload_len);

/* selftests and stats */
bool router_perform_test(router_address id);
bool router_iftest(void);
void router_stats(void);
void router_inspect_frame(const router_frame *pkt);

#endif /* CORE_SERVICES_ROUTER_H */

#ifndef CORE_SERVICES_ADDR_H
#define CORE_SERVICES_ADDR_H

#include <stdint.h>
#include <stdbool.h>
#include <core/version.h>

/* Router address Object : 4-bit class + 12-bit value
 *
 * Address      Function
 * --------------------------------------------------------
 * unicast	    fixed endpoints, mostly hardware-shaped
 * anycast	    replaceable services, found by service id
 * multicast	event streams, with subscribers
 * port	        service descriptor within a device
 *
 * Layout:
 * 15      12 11                          0
 * +---------+---------------------------+
 * |  CLASS  |          VALUE            |
 * +---------+---------------------------+
 *
 * A mask-based addressing algorithm for routing endpoints and messages.
 */

/* moved to config.h */
//#define ADDR_CLASS_SHIFT  12
//#define ADDR_VALUE_MASK   0x0FFFu


/* address atom type */
typedef uint16_t router_address;

/* address class
 *  4-bit class in bits 15..12, 12-bit value in bits 11..0):
 *
 * - UNICAST (0x0):
 *   value = endpoint ID; used for endpoint identities
 *   (and sources, including ADDR_NOBODY = 0x0000), exact single-endpoint match on dispatch.
 *
 * - MULTICAST (0x1):
 *   value = group ID (0..15); destination only, never an endpoint identity.
 *   Dispatches to all registered endpoints subscribed to that group bit.
 *
 * - BROADCAST (0x2):
 *   value = 0; destination only, targets all currently registered endpoints regardless of multicast membership.
 *
 * - ANYCAST (0x3):
 *   value = service ID; destination only (not yet routed), intended to resolve to a single provider for that service.
 *
 * - PROTECTED (0x4..0xF):
 *   protected and privileged address space; not valid as normal endpoint identities or unprivileged targets.
 */
enum addr_class {
    ADDR_CLASS_UNICAST   = 0x0,
    ADDR_CLASS_MULTICAST = 0x1,
    ADDR_CLASS_BROADCAST = 0x2,
    ADDR_CLASS_ANYCAST   = 0x3,
    ADDR_CLASS_PROTECTED = 0x4, /* 0x4..0xF purpose: clean separation from UNICAST ID space. */
};

/* constructors */
#define ADDR_MAKE(cls, val) \
    ((router_address)((((cls) & 0xFu) << ADDR_CLASS_SHIFT) | ((val) & ADDR_VALUE_MASK)))

#define ADDR_UNICAST(v)    ADDR_MAKE(ADDR_CLASS_UNICAST,   (v))
#define ADDR_MULTICAST(v)  ADDR_MAKE(ADDR_CLASS_MULTICAST, (v))
#define ADDR_ANYCAST(v)    ADDR_MAKE(ADDR_CLASS_ANYCAST,   (v))

/* reserved singletons */
#define ADDR_NOBODY        ADDR_UNICAST(NOBODY)                      /* 0x0000: source stays anonymous */
#define ADDR_BROADCAST     ADDR_MAKE(ADDR_CLASS_BROADCAST, 0)   /* 0x2000: targets everyone */
#define ADDR_ANY           ADDR_BROADCAST                       /* alias */

/* accessors */
static inline uint8_t  addr_class(router_address a) { return (uint8_t)(a >> ADDR_CLASS_SHIFT); }
static inline uint16_t addr_value(router_address a) { return (uint16_t)(a & ADDR_VALUE_MASK); }

static inline bool addr_is_unicast  (router_address a) { return addr_class(a) == ADDR_CLASS_UNICAST;   }
static inline bool addr_is_multicast(router_address a) { return addr_class(a) == ADDR_CLASS_MULTICAST; }
static inline bool addr_is_broadcast(router_address a) { return addr_class(a) == ADDR_CLASS_BROADCAST; }
static inline bool addr_is_anycast  (router_address a) { return addr_class(a) == ADDR_CLASS_ANYCAST;   }

#endif /* CORE_ARCH_ADDR_H */

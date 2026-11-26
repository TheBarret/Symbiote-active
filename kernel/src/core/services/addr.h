#ifndef CORE_SERVICES_ADDR_H
#define CORE_SERVICES_ADDR_H

#include <stdint.h>
#include <stdbool.h>
#include <core/version.h>

/* codebase: 0.2.0 */

/* Router address object : 4-bit class + 12-bit value
 *
 * Class        Function
 * --------------------------------------------------------
 * unicast      fixed endpoints, mostly hardware-shaped
 * multicast    event streams, with subscribers
 * broadcast    every registered endpoint
 * anycast      replaceable services, found by service id
 * protected    interior bus space, reserved
 *
 * Layout:
 * 15      12 11                          0
 * +---------+---------------------------+
 * |  CLASS  |          VALUE            |
 * +---------+---------------------------+
 *
 * A mask-based addressing scheme for routing endpoints and messages.
 * Ports are not part of the address: they live in router_frame.port.
 *
 * Class	Name	    Value space	            Role	Valid as
 * 0x0	    UNICAST	    0..4095 endpoint IDs	One specific endpoint. Identity + source + destination.	source, destination
 * 0x1	    MULTICAST	0..15 group IDs	        A group bit. Delivery target only.	destination only
 * 0x2	    BROADCAST	0 (ignored)	            Every registered endpoint,	destination only
 * 0x3	    ANYCAST	    0..4095                 Service IDs	One provider of a service, resolved by ID.	destination only
 * 0x4..0xF	PROTECTED	-	                    Reserved interior bus space. Currently rejected.	nothing yet
 *
 * Everything between 0x0001..0x0FFF is free address space.
 * ADDR_CLASS_SHIFT, ADDR_VALUE_MASK and NOBODY are provided by config.h.
 */

/* address atom type */
typedef uint16_t router_address;

/* address class (4-bit class in bits 15..12, 12-bit value in bits 11..0)
 *
 * - UNICAST (0x0):
 *   value = endpoint ID; used for endpoint identities and sources
 *   (including ADDR_NOBODY = 0x0000); exact single-endpoint match on dispatch.
 *
 * - MULTICAST (0x1):
 *   value = group ID (0..15); destination only, never an endpoint identity.
 *   Dispatches to all endpoints subscribed to that group bit.
 *
 * - BROADCAST (0x2):
 *   value = 0; destination only, targets all registered endpoints
 *   regardless of multicast membership.
 *
 * - ANYCAST (0x3):
 *   value = service ID; destination only, resolves to a single provider
 *   of that service (the most recently registered one).
 *
 * - PROTECTED (0x4..0xF):
 *   protected and privileged address space; not valid as endpoint identity
 *   or unprivileged target. Currently rejected as a destination.
 */
enum addr_class {
    ADDR_CLASS_UNICAST   = 0x0,
    ADDR_CLASS_MULTICAST = 0x1,
    ADDR_CLASS_BROADCAST = 0x2,
    ADDR_CLASS_ANYCAST   = 0x3,
    ADDR_CLASS_PROTECTED = 0x4, /* 0x4..0xF, kept apart from the UNICAST ID space */
};

_Static_assert(ADDR_CLASS_SHIFT + 4 == 16,
               "addr.h: class field must fill the top 4 bits of router_address");
_Static_assert(ADDR_VALUE_MASK == (1u << ADDR_CLASS_SHIFT) - 1u,
               "addr.h: ADDR_VALUE_MASK must cover exactly the bits below the class field");

/* constructors */
#define ADDR_MAKE(cls, val) \
    ((router_address)((((cls) & 0xFu) << ADDR_CLASS_SHIFT) | ((val) & ADDR_VALUE_MASK)))

#define ADDR_UNICAST(v)    ADDR_MAKE(ADDR_CLASS_UNICAST,   (v))
#define ADDR_MULTICAST(v)  ADDR_MAKE(ADDR_CLASS_MULTICAST, (v))
#define ADDR_ANYCAST(v)    ADDR_MAKE(ADDR_CLASS_ANYCAST,   (v))

/* reserved singletons */
#define ADDR_NOBODY        ADDR_UNICAST(NOBODY)             /* 0x0000: source stays anonymous */
#define ADDR_BROADCAST     ADDR_MAKE(ADDR_CLASS_BROADCAST, 0) /* 0x2000: targets everyone */
#define ADDR_ANY           ADDR_BROADCAST                   /* alias */

/* accessors */
static inline uint8_t  addr_class(router_address a) { return (uint8_t)(a >> ADDR_CLASS_SHIFT); }
static inline uint16_t addr_value(router_address a) { return (uint16_t)(a & ADDR_VALUE_MASK); }

static inline bool addr_is_unicast  (router_address a) { return addr_class(a) == ADDR_CLASS_UNICAST;   }
static inline bool addr_is_multicast(router_address a) { return addr_class(a) == ADDR_CLASS_MULTICAST; }
static inline bool addr_is_broadcast(router_address a) { return addr_class(a) == ADDR_CLASS_BROADCAST; }
static inline bool addr_is_anycast  (router_address a) { return addr_class(a) == ADDR_CLASS_ANYCAST;   }
static inline bool addr_is_protected(router_address a) { return addr_class(a) >= ADDR_CLASS_PROTECTED; }

#endif /* CORE_SERVICES_ADDR_H */

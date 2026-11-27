#ifndef CORE_SERVICES_ADDR_H
#define CORE_SERVICES_ADDR_H

#include <stdint.h>
#include <stdbool.h>
#include <core/version.h>

/* codebase: 0.2.0 */

/* Router address: 4-bit class (bits 15..12) + 12-bit value (bits 11..0).
 * Ports are not part of the address: they live in router_frame.port.
 *
 * Class      Value      Valid as     Meaning
 * UNICAST    0..4095    src, dst     one endpoint; NOBODY (0) is the anonymous source
 * MULTICAST  0..15      dst only     every endpoint subscribed to that group bit
 * BROADCAST  ignored    dst only     every registered endpoint
 * ANYCAST    1..4095    dst only     newest provider bound to that service id (0 = none)
 * PROTECTED  0x4..0xF   nothing yet  reserved interior bus space, rejected
 *
 * ADDR_CLASS_SHIFT, ADDR_VALUE_MASK and NOBODY come from config.h.
 */

typedef uint16_t router_address;

enum addr_class {
    ADDR_CLASS_UNICAST   = 0x0,
    ADDR_CLASS_MULTICAST = 0x1,
    ADDR_CLASS_BROADCAST = 0x2,
    ADDR_CLASS_ANYCAST   = 0x3,
    ADDR_CLASS_PROTECTED = 0x4, /* 0x4..0xF */
};

_Static_assert(ADDR_CLASS_SHIFT + 4 == 16,
               "addr.h: class field must fill the top 4 bits of router_address");
_Static_assert(ADDR_VALUE_MASK == (1u << ADDR_CLASS_SHIFT) - 1u,
               "addr.h: ADDR_VALUE_MASK must cover exactly the bits below the class field");

#define ADDR_MAKE(cls, val) \
    ((router_address)((((cls) & 0xFu) << ADDR_CLASS_SHIFT) | ((val) & ADDR_VALUE_MASK)))

#define ADDR_UNICAST(v)    ADDR_MAKE(ADDR_CLASS_UNICAST,   (v))
#define ADDR_MULTICAST(v)  ADDR_MAKE(ADDR_CLASS_MULTICAST, (v))
#define ADDR_ANYCAST(v)    ADDR_MAKE(ADDR_CLASS_ANYCAST,   (v))

#define ADDR_NOBODY        ADDR_UNICAST(NOBODY)               /* 0x0000, anonymous source */
#define ADDR_BROADCAST     ADDR_MAKE(ADDR_CLASS_BROADCAST, 0) /* 0x2000, everyone */
#define ADDR_ANY           ADDR_BROADCAST                     /* alias of BROADCAST, not ANYCAST */

static inline uint8_t  addr_class(router_address a) { return (uint8_t)(a >> ADDR_CLASS_SHIFT); }
static inline uint16_t addr_value(router_address a) { return (uint16_t)(a & ADDR_VALUE_MASK); }

static inline bool addr_is_unicast  (router_address a) { return addr_class(a) == ADDR_CLASS_UNICAST;   }
static inline bool addr_is_multicast(router_address a) { return addr_class(a) == ADDR_CLASS_MULTICAST; }
static inline bool addr_is_broadcast(router_address a) { return addr_class(a) == ADDR_CLASS_BROADCAST; }
static inline bool addr_is_anycast  (router_address a) { return addr_class(a) == ADDR_CLASS_ANYCAST;   }
static inline bool addr_is_protected(router_address a) { return addr_class(a) >= ADDR_CLASS_PROTECTED; }

#endif /* CORE_SERVICES_ADDR_H */

#ifndef CORE_SERVICES_ADDR_H
#define CORE_SERVICES_ADDR_H

#include <stdint.h>
#include <stdbool.h>
#include <core/version.h>

/* Router address: 4-bit class + 12-bit value.
 *
 *  15      12 11                          0
 * +---------+---------------------------+
 * |  CLASS  |          VALUE            |
 * +---------+---------------------------+
 */

typedef uint16_t router_address;

/* address classes */
enum addr_class {
    ADDR_CLASS_UNICAST   = 0x0,   /* value = endpoint ID         */
    ADDR_CLASS_MULTICAST = 0x1,   /* value = group ID (0..15)    */
    ADDR_CLASS_BROADCAST = 0x2,   /* value = 0 (all endpoints)   */
    ADDR_CLASS_ANYCAST   = 0x3,   /* value = service ID          */
    ADDR_CLASS_RESERVED  = 0x4,   /* .. 0xF reserved             */
};

/* moved to config.h */
//#define ADDR_CLASS_SHIFT  12
//#define ADDR_VALUE_MASK   0x0FFFu

/* constructors */
#define ADDR_MAKE(cls, val) \
    ((router_address)((((cls) & 0xFu) << ADDR_CLASS_SHIFT) | ((val) & ADDR_VALUE_MASK)))

#define ADDR_UNICAST(v)    ADDR_MAKE(ADDR_CLASS_UNICAST,   (v))
#define ADDR_MULTICAST(v)  ADDR_MAKE(ADDR_CLASS_MULTICAST, (v))
#define ADDR_ANYCAST(v)    ADDR_MAKE(ADDR_CLASS_ANYCAST,   (v))

/* reserved singletons */
#define ADDR_NOBODY        ADDR_UNICAST(0)                      /* 0x0000: source stays anonymous */
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

#ifndef CORE_SERVICES_ROUTER_H
#define CORE_SERVICES_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* network packet types */
typedef enum {
    PACKET_TYPE_DATA     = 0,   /* raw byte data */
    PACKET_TYPE_SIGNAL   = 1,   /* signal packet, state, ping, etc..  */
} network_ptypes;

/* network packet frame */
typedef struct {
    uint32_t src_addr;      // source address
    uint32_t dst_addr;      // destination address
    uint16_t port;          // Logical endpoint (0-65535)
    network_ptypes type;    // packet type
    size_t   length;        // length of payload
    uint8_t  payload[];     // payload
} __attribute__((packed)) network_pframe;

// callback signature
typedef void (*network_rx_callback_t)(network_pframe *pkt);

// phase identifiers
enum network_phases {
    NET_PHASE_POLL   = 0,   // on input, flush TX buffers
    NET_PHASE_ROUTE  = 1,   // routing, delivery
    NET_PHASE_SYNC   = 2,   // cleanup, post-dispatch sync
};

typedef struct network_device {
    const char *name;                // human-readable string name
    uint32_t   id;                   // Unique address
    uint64_t   address_base;         // local address (logical mapped)
    size_t     buffer_length;        // length working buffer
    volatile uint8_t *buffer;        // buffer pointer (allocated via kmalloc)
    network_rx_callback_t rx_handler;  // called during dispatch phase, packet arrives
    struct network_device *next;     // linked list pointer for lookup
} network_device_t;

// router API
void router_init(void);
bool router_register_device(const char *name, uint32_t id, uint64_t address_base, size_t buffer_length, network_rx_callback_t handler);
bool router_send_packet(network_pframe *pkt);
void router_cycle();

#endif

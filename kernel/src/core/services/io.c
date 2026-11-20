#include <lib/mem.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/router.h>

/* console sink: payload as text. Payload is not NUL-terminated, so copy it. */
static void route_to_console(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_DATA) return;

    char buf[LOG_SIZE]; /* config.h */
    size_t cap = sizeof(buf) - 1u;
    size_t len = pkt->length < cap ? pkt->length : cap;

    memcpy(buf, pkt->payload, len);
    buf[len] = '\0'; /* append null */

    if (pkt->length > cap) {
        kprintf("%s...(truncated: showing %u of %u bytes)\n",
                buf, (unsigned)len, (unsigned)pkt->length);
    } else {
        kprintf("%s\n", buf);
    }
}

/* console sink: payload as hex; 1/2/4-byte payloads also as little-endian decimal. */
static void route_to_consolev(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_DATA) return;
    if (pkt->length == 1u || pkt->length == 2u || pkt->length == 4u) {
        uint32_t v = 0;
        for (uint16_t i = 0; i < pkt->length; i++) {
            v |= (uint32_t)pkt->payload[i] << (8u * i);
        }
        kprintf("%u", (unsigned)v);
    }
    kprintf("\n");
}


void init_io(router_address id1, router_address id2) {
    register_sink("io", id1, route_to_console);
    register_sink("iov", id2, route_to_consolev);
}

#include <lib/mem.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/dev/vfs.h>
#include <core/services/router.h>

/* ascii greeter */
static const char t_ascii[] =
        "┌┼───────────────────────────────────────────────────────────────────────────────┐\n"
        "││      ___             _     _       _                                          │\n"
        "││     / __|_  _ _ __  | |__ (_) ___ | |_  ___                                   │\n"
        "││     \\__ \\ || | '  \\ | '_ \\| |/ _ \\|  _|/ -_)                                  │\n"
        "││     |___/\\_, |_|_|_||_.__/|_|\\___/ \\__|\\___|OS                                │\n"
        "││           |__/x86-64                                                          │\n"
        "└┼───────────────────────────────────────────────────────────────────────────────┘\n";

/* console sink: payload as text. Payload is not NUL-terminated, so copy it. */
static void io_console(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_DATA) return;
    char buf[LOG_SIZE]; /* config.h */
    size_t cap = sizeof(buf) - 1u;
    size_t len = pkt->length < cap ? pkt->length : cap;
    memcpy(buf, pkt->payload, len);
    buf[len] = '\0'; /* append null */
    if (pkt->length > cap) {
        kprintf("%s..(truncated:%u/%u bytes)\n",
                buf, (unsigned)len, (unsigned)pkt->length);
    } else {
        kprintf("%s", buf);
    }
}

/* console sink: payload as hex; 1/2/4-byte payloads also as little-endian decimal. */
static void io_console_value(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_DATA) return;
    if (pkt->length == 1u || pkt->length == 2u || pkt->length == 4u) {
        uint32_t v = 0;
        for (uint16_t i = 0; i < pkt->length; i++) {
            v |= (uint32_t)pkt->payload[i] << (8u * i);
        }
        kprintf("%u", (unsigned)v);
    }
}

static void io_demux(router_frame *pkt) {
    switch (pkt->port) {
    case 0: io_console(pkt);  break;
    case 1: io_console_value(pkt); break;
    default: break;
    }
}
/* connects console sink, send visual greeter for confirmation */
void init_io(router_address id1, router_address id2) {
    router_hotwire(id1, io_demux);
    router_hotwire(id2, io_demux);
    router_send(router_create_p(ADDR_NOBODY, id1, FRAME_DATA, 0, (const uint8_t *)t_ascii, (uint16_t)(sizeof(t_ascii) - 1u)));
}

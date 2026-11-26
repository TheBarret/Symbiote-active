#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/random.h>
#include <lib/string.h>
#include <core/arch/cpu.h>
#include <core/dev/timer.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/loop.h>
#include <core/services/addr.h>
#include <core/services/router.h>
#include <core/services/decoder.h>
#include <core/services/logger.h>

/* clear screen */

static void clear_demux(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    if (pkt->port == DEFAULT_PORT) {
        kprintf("\x1b[2J\x1b[H");
        return;
    }
}

/* stop machine */
static void shutdown_demux(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    if (pkt->port == DEFAULT_PORT) {
        kprintf("Bye!\n");
        cpu_halt_forever();
    }
}

/* randomizer */

static void randomizer_respond(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    uint16_t value = rnd_next();
    uint8_t payload[DECODER_HEADER_SIZE + sizeof value];
    uint16_t length;
    if (frame_encode(DTYPE_UINT16, 0, &value, sizeof value, payload, sizeof payload, &length) != DECODER_OK) return;
    router_frame *reply = router_create_p(ADDR_UNICAST(NOBODY), pkt->src_addr, FRAME_STRUCT, DEFAULT_PORT, payload, length);
    if (!reply || !router_send(reply)) { klog_warning("router rejected frame\n"); }
}

static void randomizer_demux(router_frame *pkt) {
    if (!pkt) return;
    switch (pkt->port) {
    case DEFAULT_PORT: randomizer_respond(pkt); break;
    default: break;
    }
}

/* sleep */

#define SLEEP_SLICE_MS  UINT64_C(20)

static const struct {
    uint8_t  port;
    uint64_t ticks;
} sleep_map[] = {
    { 1, UINT64_C(100)  },
    { 2, UINT64_C(200)  },
    { 3, UINT64_C(300)  },
    { 4, UINT64_C(400)  },
    { 5, UINT64_C(500)  },
};

static int sleep_process(uint64_t ticks) {
    if (ticks == 0) return 1;
    while (ticks) {
        uint64_t slice = ticks > SLEEP_SLICE_MS ? SLEEP_SLICE_MS : ticks;
        timer_sleep_ms(slice);
        ticks -= slice;
    }
    return 0;
}

static void sleep_demux(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL || pkt->port == 0) return;
    for (size_t i = 1; i < ARRAY_SIZE(sleep_map); ++i) {
        if (sleep_map[i].port == pkt->port) {
            sleep_process(sleep_map[i].ticks);
            return;
        }
    }
}

/* console */


static void io_console(router_frame *pkt) {
    if (!pkt) return;
    if (pkt->type == FRAME_DATA) {
        char buf[ROUTER_SIZE_MAX]; /* config.h */
        size_t cap = sizeof(buf) - 1u;
        size_t len = pkt->length < cap ? pkt->length : cap;
        memcpy(buf, pkt->payload, len);
        buf[len] = '\0'; /* always */
        if (pkt->length > cap) {
            kprintf("%s..(truncated:%u/%u bytes)\n",
                    buf, (unsigned)len, (unsigned)pkt->length);
        } else {
            kprintf("%s", buf);
        }
        return;
    }
    if (pkt->type == FRAME_STRUCT) {
        if (frame_check(pkt->payload, pkt->length) == DECODER_OK) {
            decoder_data frame_info;
            frame_decode(pkt->payload, pkt->length, &frame_info);

            switch (frame_info.dtype) {
                case DTYPE_BYTE:
                case DTYPE_UINT16:
                case DTYPE_UINT32: {
                    uint32_t v = 0;
                    for (size_t i = 0; i < frame_info.len; i++) {
                        v |= (uint32_t)frame_info.data[i] << (8u * i);
                    }
                    kprintf("%u", (unsigned)v);
                    break;
                }
                case DTYPE_INT16: {
                    uint16_t raw = (uint16_t)frame_info.data[0] |
                                   ((uint16_t)frame_info.data[1] << 8u);
                    kprintf("%d", (int)(int16_t)raw);
                    break;
                }
                case DTYPE_INT32: {
                    uint32_t raw = 0;
                    for (size_t i = 0; i < 4; i++) {
                        raw |= (uint32_t)frame_info.data[i] << (8u * i);
                    }
                    kprintf("%d", (int)(int32_t)raw);
                    break;
                }
                case DTYPE_BOOL:
                    kprintf("%s", frame_info.data[0] ? "true" : "false");
                    break;

                case DTYPE_STRING: {
                    char tmp[DECODER_STRING_MAX + 1];
                    size_t n = frame_info.len < DECODER_STRING_MAX ? frame_info.len : DECODER_STRING_MAX;
                    memcpy(tmp, frame_info.data, n);
                    tmp[n] = '\0'; /* always */
                    kprintf("%s", tmp);
                    break;
                }

                default:
                    break;
            }
        }
        return;
    }
}

static void io_demux(router_frame *pkt) {
    if (!pkt) return;
    switch (pkt->port) {
    case DEFAULT_PORT: io_console(pkt);  break;
    default: break;
    }
}

/* loop manager */

static void lp_reply(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    uint16_t r_length;
    struct loop_stats r_stats = loop_get_stats();
    uint32_t r_value = r_stats.tasks;
    uint8_t r_payload[DECODER_HEADER_SIZE + sizeof r_value];
    /* cast type and validate */
    if (frame_encode(DTYPE_UINT32, 0,
                 &r_value, sizeof r_value,
                 r_payload, sizeof r_payload, &r_length) != DECODER_OK) return;
    /* cast frame */
    router_frame *reply = router_create_p(ADDR_UNICAST(NOBODY), pkt->src_addr,
                                          FRAME_STRUCT, DEFAULT_PORT,
                                          r_payload, r_length);
    /* hand off */
    if (!reply || !router_send(reply)) { klog_warning("router rejected frame\n"); }
}

static void lp_demux(router_frame *pkt) {
    if (!pkt) return;
    switch (pkt->port) {
    case DEFAULT_PORT: lp_reply(pkt);  break;
    default: break;
    }
}

/* init */
void router_register_defaults() {
    router_register("print", ADDR_UNICAST(BUS_PRINT), io_demux);
    router_register("rnd",   ADDR_UNICAST(BUS_RND), randomizer_demux);
    router_register("sleep", ADDR_UNICAST(BUS_SLEEP), sleep_demux);
    router_register("cls",   ADDR_UNICAST(BUS_CLEAR), clear_demux);
    router_register("exit",  ADDR_UNICAST(BUS_KILL), shutdown_demux);
    router_register("tasks",  ADDR_UNICAST(BUS_TASKS), lp_demux);
}

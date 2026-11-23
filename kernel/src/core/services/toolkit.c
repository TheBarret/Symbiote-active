#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <core/arch/cpu.h>
#include <core/dev/timer.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/router.h>

/* clear screen */
static void clear_demux(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    if (pkt->port == 1) {
        kprintf("\x1b[2J\x1b[H");
        return;
    }
}

/* stop machine */
static void shutdown_demux(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    if (pkt->port == 1) {
        kprintf("Bye!\n");
        cpu_halt_forever();
    }
}

/* sleep */

#define SLEEP_SLICE_MS  UINT64_C(20)

static const struct {
    uint8_t  port;
    uint64_t ticks;
} sleep_map[] = {
    { 0, UINT64_C(100)  },
    { 1, UINT64_C(250)  },
    { 2, UINT64_C(500)  },
    { 3, UINT64_C(1000) },
    { 4, UINT64_C(2000) },
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
    if (!pkt || pkt->type != FRAME_SIGNAL) return;

    for (size_t i = 0; i < ARRAY_SIZE(sleep_map); ++i) {
        if (sleep_map[i].port == pkt->port) {
            sleep_process(sleep_map[i].ticks);
            return;
        }
    }
}
void init_toolkit(router_address id1, router_address id2, router_address id3) {
    router_register("clear", id1, clear_demux);
    router_register("shutdown", id2, shutdown_demux);
    router_register("sleep", id3, sleep_demux);
}

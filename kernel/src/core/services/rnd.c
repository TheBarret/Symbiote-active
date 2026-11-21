#include <lib/random.h>
#include <core/klog.h>
#include <core/services/rnd.h>
#include <core/services/addr.h>
#include <core/services/router.h>

static void randomizer_fill_bytes(uint8_t *dst, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = (uint8_t)(rnd_next() & 0x00FFu);
    }
}

static void randomizer_respond(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;

    router_address requester = pkt->src_addr;
    uint8_t response[RND_MAXSIZE];
    randomizer_fill_bytes(response, RND_MAXSIZE);

    router_frame *reply = router_create_p(ADDR_NOBODY, requester,
                                        FRAME_DATA, 0, response, RND_MAXSIZE);
    if (!reply) {
        klog_warning("randomizer_respond() failed to create frame\n");
        return;
    }
    if (!router_send(reply)) {
        klog_warning("randomizer_respond() frame dropped (no route to 0x%x)\n", (unsigned)requester);
    }
}

static void randomizer_demux(router_frame *pkt) {
    if (!pkt) return;
    switch (pkt->port) {
    case 0: randomizer_respond(pkt); break;
    default: break;
    }
}

void init_rnd(router_address id) {
    rnd_init(RND_SEED);
    router_hotwire(id, randomizer_demux);
}

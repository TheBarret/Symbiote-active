#include <lib/random.h>
#include <core/klog.h>
#include <core/services/rnd.h>
#include <core/services/router.h>

static void randomizer_fill_bytes(uint8_t *dst, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = (uint8_t)(rnd_next() & 0x00FFu);
    }
}

static void route_to_randomizer(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    router_address requester = pkt->src_addr;
    uint8_t response[RND_MAXSIZE];

    randomizer_fill_bytes(response, RND_MAXSIZE);

    router_frame *reply = router_create(ADDR_UNICAST(3), requester, FRAME_DATA,
                                        response, RND_MAXSIZE);
    if (!reply) {
        klog_warning("route_to_randomizer() failed to create reply frame\n");
        return;
    }
    if (!router_send(reply)) {
        klog_warning("route_to_randomizer() no route back to 0x%x\n", (unsigned)requester);
    }
}

void init_rnd(router_address id) {
    rnd_init(RND_SEED);
    register_sink("rnd", id, route_to_randomizer);
}

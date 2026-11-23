#include <lib/random.h>
#include <core/klog.h>
#include <core/services/router.h>
#include <core/services/addr.h>
#include <core/services/decoder.h>
#include <core/services/rnd.h>

static router_address g_device_id; /* endpoint address */

static void randomizer_respond(router_frame *pkt) {
    if (!pkt || pkt->type != FRAME_SIGNAL) return;
    uint16_t value = rnd_next();
    uint8_t payload[DECODER_HEADER_SIZE + sizeof value];
    uint16_t length;
    if (frame_encode(DTYPE_UINT16, 0, &value, sizeof value, payload, sizeof payload, &length) != DECODER_OK) return;
    router_frame *reply = router_create_p(g_device_id, pkt->src_addr, FRAME_STRUCT, 0, payload, length);
    if (!reply || !router_send(reply)) { klog_warning("randomizer failed, bad reply object\n"); }
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
    g_device_id = id;
    router_register("rnd", id, randomizer_demux);
}

#include <lib/mem.h>
#include <core/klog.h>
#include <core/arch/heap.h>
#include <core/services/router.h>

static void sink_rx(network_pframe *pkt) {
    klog("  network_pframe() src=0x%x, port=%u, length=%u\n",
         pkt->src_addr, pkt->port, (unsigned)pkt->length);
    if (pkt->type == PACKET_TYPE_SIGNAL && pkt->length > 0) {
        klog("  payload: '%.*s'\n", (int)pkt->length, pkt->payload);
    }
}

void run_echo(void) {
    bool ok = router_register_device("echo",0x10,0x2000,512,sink_rx);
    if (!ok) {
        klog_error("echo: failed to register sink\n");
        return;
    }
    const char *msg = "ECHO";
    size_t msg_len = 4;
    size_t total_size = sizeof(network_pframe) + msg_len;
    network_pframe *pkt = (network_pframe *)kmalloc(total_size);
    if (!pkt) {
        klog_error("echo: failed to allocate test packet\n");
        return;
    }
    pkt->src_addr = 0x01;
    pkt->dst_addr = 0x10;
    pkt->port     = 100;
    pkt->type     = PACKET_TYPE_SIGNAL;
    pkt->length   = msg_len;
    memcpy(pkt->payload, msg, msg_len);
    klog("  sending payload...\n");
    router_send_packet(pkt);
}

#include <core/services/router.h>

static void route_to_void(router_frame *pkt) {
    (void)pkt;
}
void init_vsink(router_address id) {
    register_sink("void", id, route_to_void);
}

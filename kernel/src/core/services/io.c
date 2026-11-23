#include <lib/mem.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/services/decoder.h>
#include <core/services/router.h>

/* console sink */
static void io_console(router_frame *pkt) {
    if (!pkt) return;

    if (pkt->type == FRAME_DATA) {
        char buf[LOG_SIZE]; /* config.h */
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
    case 0: io_console(pkt);  break;
    default: break;
    }
}
void init_io(router_address id) {
    router_register("print", id, io_demux);
}

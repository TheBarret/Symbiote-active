#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/random.h>
#include <core/version.h>
#include <core/panic.h>
#include <core/klog.h>

/* codebase: 0.2.0 */

static uint16_t g_state;
static uint8_t  g_ring[RND_RING_SIZE];
static uint32_t g_head;            /* next write index, 0..RND_RING_SIZE-1 */
static uint64_t g_count;           /* total bytes ever generated */

void rnd_init(uint16_t seed) {
    if (seed == 0) {
        klog_warning("rnd_init() seed value cannot be zero, using fallback...\n");
        g_state = (uint16_t)RND_SEED_ALT;
    } else {
        g_state = seed;
    }
    memset(g_ring, 0, sizeof g_ring);
    g_head  = 0;
    g_count = 0;
}

uint16_t rnd_next(void) {
    uint16_t x = g_state;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    g_state = x;

    g_ring[g_head] = (uint8_t)(x & 0xFFu);
    g_head = (g_head + 1u) % RND_RING_SIZE;
    g_count++;

    return x;
}

uint8_t rnd_ring_byte(uint32_t off) {
    if (off >= RND_RING_SIZE) return 0;
    return g_ring[off];
}

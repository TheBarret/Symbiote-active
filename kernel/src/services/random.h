#ifndef CORE_SERVICES_RANDOM_H
#define CORE_SERVICES_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

/* codebase: 0.2.0 */

/* Randomizer
 * Deterministic, non-cryptographic pseudo-random source.
 * Access paths:
 *   - rnd_next() / rnd_ring_byte(): direct, for in-kernel callers (VM core, tests). Bypasses the bus.
 *   - get_rnd(): the isolated ISA endpoint. Read-only, W8 and W16.
 *
 * Reads never advance the stream. rnd_next() is the only thing that does.
 */

/* Initialize the device. Seed 0 is a fixed point of xorshift16 and is rejected (PANIC).
 * Double-call re-seeds and resets the ring.
 */
void rnd_init(uint16_t seed);

/* Advance the PRNG by one step, push the low byte into the ring (overwriting
 * the oldest), and return the full 16-bit word. */
uint16_t rnd_next(void);

/* Pure read of the ring at byte offset `off`. Returns 0 if off >= RND_RING_SIZE.
 * No side effects; safe to call from anywhere, any number of times. */
uint8_t rnd_ring_byte(uint32_t off);

#endif /* CORE_RND_H */

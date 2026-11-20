#ifndef CORE_LIB_RANDOM_H
#define CORE_LIB_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

/* codebase: 0.2.0 */
/* Randomizer: Deterministic, non-cryptographic pseudo-random source. */

/* Initializer, seed 0 rejected, double-call re-seeds and resets the ring. */
void rnd_init(uint16_t seed);

/* Advance the PRNG by one step, push low byte into the ring (overwriting), and return the full 16-bit word. */
uint16_t rnd_next(void);

/* returns 0 if off >= RND_RING_SIZE */
uint8_t rnd_ring_byte(uint32_t off);

#endif /* CORE_RND_H */

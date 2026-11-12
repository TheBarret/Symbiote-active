#ifndef EXT_STRESSER_H
#define EXT_STRESSER_H

#include <stddef.h>
#include <stdint.h>
#include <core/mask.h>

/* stresser: writes and clears stress entries in the env store.
 *
 * The extension registers one command, `stresser`, with subcommands:
 *
 *   stresser fill <n>      write n randomized entries into the stress namespace
 *   stresser clear         drop the stress namespace
 *
 * The stress namespace and its mask taxonomy are fixed so that `fill` and `stats` (in envtool) agree on meaning.
 * Every stress entry carries exactly one TTL bit, one budget bit, one importance bit,
 * and optionally one or both of {PINNED, SECRET} so the flag enforcement paths are exercised.
 *
 * Cross-queries (e.g. "short ttl AND low importance") are expressed by OR-ing the relevant bits into a single query_mask,
 * same as the count_by_mask contract in env.h.
 *
 * Context: thread context only. Do not call from an ISR.
 */

#define STRESSER_NS "stress"

/* PRIVATE-field taxonomy (bits 0..15), used as subset-match bits. */
#define STRESSER_SHORT_TTL   (1u << 0)   /* expires in ~1s */
#define STRESSER_LONG_TTL    (1u << 1)   /* expires in ~30s */
#define STRESSER_NO_TTL      (1u << 2)   /* persistent */
#define STRESSER_LOW_BUDGET  (1u << 3)   /* 1..2 gets */
#define STRESSER_NO_BUDGET   (1u << 4)   /* unlimited */
#define STRESSER_LOW_IMP     (1u << 5)   /* importance 0 */
#define STRESSER_HIGH_IMP    (1u << 6)   /* importance MAX */

/* FLAGS bits (bits 16..19), from mask.h — used verbatim. */
#define STRESSER_PINNED      ENV_FLAG_PINNED
#define STRESSER_SECRET      ENV_FLAG_SECRET

/* Populate the stress namespace with `n` entries. Returns the number of
 * successful set() calls; failures are reported on the console. */
int stresser_fill(int n);

/* Drop the entire stress namespace. Returns 0 on success. */
int stresser_clear(void);

#endif /* EXT_STRESSER_H */

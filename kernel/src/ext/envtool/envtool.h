#ifndef EXT_ENVTOOL_H
#define EXT_ENVTOOL_H

#include <stddef.h>
#include <stdint.h>

/* envtool: inspection and stress utilities for the environment store.
 *
 * The extension registers one command, `envtool`, with subcommands:
 *
 *   envtool [inspect]      interactive TUI viewer (default)
 *   envtool fill <n>       write n randomized entries into the stress namespace
 *   envtool clear          drop the stress namespace
 *   envtool stats          per-category counts + device snapshot
 *
 * The stress namespace and its mask taxonomy are fixed so that `fill` and
 * `stats` agree on meaning. Every stress entry carries exactly one TTL bit,
 * one budget bit, and one importance bit; the remaining bits are reserved
 * for future axes. Cross-queries (e.g. "short ttl AND low importance") are
 * expressed by OR-ing the relevant bits into a single query_mask.
 *
 * Consumers that want to drive the same stress load programmatically can
 * call envtool_fill() / envtool_clear() / envtool_stats() directly; the
 * command wrappers add argument parsing and nothing else.
 *
 * Context: thread context only. Do not call from an ISR. envtool_fill()
 * writes to the environment store and therefore has the same reentrancy
 * rules as env itself.
 */

#define ENVTOOL_NS "stress"

#define ENVTOOL_SHORT_TTL   (1u << 0)   /* expires in ~1s */
#define ENVTOOL_LONG_TTL    (1u << 1)   /* expires in ~30s */
#define ENVTOOL_NO_TTL      (1u << 2)   /* persistent */
#define ENVTOOL_LOW_BUDGET  (1u << 3)   /* 1..2 gets */
#define ENVTOOL_NO_BUDGET   (1u << 4)   /* unlimited */
#define ENVTOOL_LOW_IMP     (1u << 5)   /* importance 0 */
#define ENVTOOL_HIGH_IMP    (1u << 6)   /* importance MAX */

/* Populate the stress namespace with `n` entries carrying randomized
 * values, TTLs, access budgets, importances, and the mask bits above.
 * Replaces existing entries with the same name. Returns the number of
 * successful set() calls; failures are reported on the console.
 * A non-positive `n` is clamped to a small default. */
int envtool_fill(int n);

/* Drop the entire stress namespace. Returns 0 on success (including the
 * case where the namespace was already empty), non-zero on device error. */
int envtool_clear(void);

/* Print a device snapshot plus per-category counts for the mask bits
 * above. Read-only; does not modify the store. */
int envtool_stats(void);

#endif /* EXT_ENVTOOL_H */

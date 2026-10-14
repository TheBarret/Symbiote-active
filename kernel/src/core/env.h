#ifndef CORE_ENV_H
#define CORE_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/bus.h>

/* Symbiote environment store ("ring collector")
 *
 * A bounded, fixed-capacity table of (namespace, name) -> value entries.
 * Each entry carries three per-entry fields:
 *
 *   access budget   how many more consuming reads it will tolerate
 *   time budget     wall-clock lifetime, in timer ticks
 *   importance      0 = keep longest, 10 = evict first; 5 = default
 *
 * Entries are reclaimed by four triggers, all funneling through one
 * internal reclaim path:
 *
 *   access exhaustion   synchronous, at the consuming read site
 *   time expiry         asynchronous, by the sweeper (if a timer exists)
 *   space pressure      synchronous, at set() time, by importance
 *   explicit removal    synchronous, via unset()
 *
 * Contract
 *
 * The five rules the caller may rely on:
 *
 *   1. set() is a full declaration. Every call replaces the entry entirely:
 *      value and metadata. There is no "update only the value" variant.
 *      The caller declares the entry's whole contract every time, or accepts the defaults.
 *
 *   2. set() is atomic at the (namespace, name) key.
 *      No intermediate state is observable, even if an entry existed before.
 *
 *   3. get() consumes one access; peek() does not.
 *      The choice is the caller's, and the two verbs map to the two patterns,
 *      (consume-once vs. inspect-without-consuming).
 *
 *   4. An entry whose time budget has passed behaves as absent,
 *      whether or not the device has physically reclaimed it.
 *      The sweeper is invisible.
 *
 *   5. Entries may be reclaimed at any time under space pressure,
 *      ordered by importance (higher evicted first), with ties broken deterministically.
 *      A caller must not assume an entry persists past its declared budgets.
 *
 * The three things the caller must NOT do:
 *
 *   - Hold pointers into the device's storage across calls.
 *     get() and peek() return a pointer valid until the next mutating call on the same device.
 *     Copy immediately if you need to keep the value.
 *
 *   - Depend on the sweeper running. The device is fully functional without a timer;
 *     the sweeper is a memory-pressure optimization, not a correctness requirement.
 *     Without a timer, entries are reclaimed only by the other three triggers.
 *
 *   - Store handles, pointers, or resources. Entries are strings.
 *     Resources have their own lifetime and belong in the device that owns them.
 *
 * Threading: single-threaded. All calls come from the shell context.
 * The sweeper also runs in shell context (via timer_drain). No locking.
 *
 * Namespaces: a string tag on each entry, not a first-class object.
 * A namespace exists iff it has at least one live entry.
 * There is no namespace table, no namespace handle, no namespace metadata.
 *
 * Namespace naming convention
 *
 * By convention, namespaces are dotted domain paths:
 *
 *   "boot.startup"    the boot sequence's startup keys
 *   "boot.shutdown"   the boot sequence's shutdown keys
 *   "user.<script>"   a script's private keys
 *   "shared"          the explicitly-shared namespace
 *
 * The device does not parse namespaces. It stores them as opaque strings and matches them exactly.
 * The convention exists so callers agree on shape; the device does not enforce it.
 */

enum env_result {
    ENV_OK              =  0,
    ENV_NOT_FOUND       = -1,   /* no such entry */
    ENV_EXPIRED         = -2,   /* entry existed, its time budget passed */
    ENV_ACCESS_EXHAUSTED= -3,   /* entry existed, its access budget was 0 */
    ENV_NO_SPACE        = -4,   /* set() could not make room */
    ENV_INVALID_ARGS    = -5,   /* NULL args, empty name, oversized value */
    ENV_NO_DEVICE       = -6,   /* bus_env() returned NULL */
    ENV_INTERNAL        = -7,   /* invariant violation; caller should log */
};

/* Default importance for set() when not specified. Midpoint of 0..10,
 * so that "more disposable" and "less disposable" are both expressible relative to an untagged entry. */
#define ENV_IMPORTANCE_DEFAULT  5

/* The scale is disposability, not value:
 *   0  = keep longest under pressure
 *   5  = default
 *   10 = evict first under pressure
 *
 * Importance only matters under space pressure.
 * The other three reclamation triggers are importance-blind:
 * an entry with importance 0 can still die from access exhaustion, time expiry, or unset().
 */
#define ENV_IMPORTANCE_MIN      0
#define ENV_IMPORTANCE_MAX      10

/* Capacity defaults. Chosen small because the device is meant to be a coordination surface,
 * not a database. Tune at ring_init() time. */
#define ENV_DEFAULT_MAX_ENTRIES 256
#define ENV_DEFAULT_MAX_BYTES   (64 * 1024)
#define ENV_NAME_MAX            64
#define ENV_VALUE_MAX           1024

/* Sweeper period and bounded work per sweep.
 * The sweeper examines at most ENV_SWEEP_BATCH entries per invocation,
 * so a large table does not stall the CPU in a single tick.
 * Same discipline as the SBasic iteration budget and the keyboard ring buffer. */
#define ENV_SWEEP_PERIOD_MS     1000
#define ENV_SWEEP_BATCH         64

/* ops struct                                                          */
/*
 * All functions return an enum env_result unless noted.
 *
 * Namespace and name arguments are NOT NULL and NOT empty. The device
 * returns ENV_INVALID_ARGS if they are. Value is NOT NULL.
 *
 * set():
 *   Creates or fully replaces the entry at (ns, name).
 *   The old entry, if any, is reclaimed atomically.
 *   Metadata is taken from the call; omitted parameters use the defaults.
 *
 *   access_budget == 0 means "unlimited consuming reads."
 *   ttl_ticks    == 0 means "no time budget."
 *   importance   is clamped to [ENV_IMPORTANCE_MIN, ENV_IMPORTANCE_MAX].
 *
 * get():
 *   Returns the value via *out (pointer valid until the next mutating call on this device).
 *   Decrements the access budget. Returns ENV_EXPIRED or ENV_ACCESS_EXHAUSTED if the entry is logically dead.
 *
 * peek():
 *   Same as get(), but does not decrement the access budget.
 *
 * unset():
 *   Removes the entry if live. Returns ENV_NOT_FOUND if it was not present (including logically expired).
 *
 * is_exist():
 *   Non-consuming existence check. Returns ENV_OK if the entry is logically live, ENV_NOT_FOUND otherwise.
 *   Never returns ENV_EXPIRED (expired == absent by rule 4).
 *
 * list():
 *   Enumerates names in a namespace. *iter is caller-owned, starts at 0, incremented by the device.
 *   Returns ENV_OK and writes the next name into name_out (null-terminated, capacity name_max).
 *   Returns ENV_NOT_FOUND when iteration is complete.
 *
 * drop_namespace():
 *   Removes every entry in the namespace. Returns ENV_OK even if the namespace was empty.
 */
struct env_ops {
    int (*set)(const char *ns, const char *name, const char *value,
               uint32_t access_budget, uint64_t ttl_ticks,
               uint8_t importance);

    int (*get)(const char *ns, const char *name, const char **out);
    int (*peek)(const char *ns, const char *name, const char **out);

    int (*unset)(const char *ns, const char *name);
    int (*is_exist)(const char *ns, const char *name);

    int (*list)(const char *ns, size_t *iter,
                char *name_out, size_t name_max);

    int (*drop_namespace)(const char *ns);
};

/* Typed finder, same shape as bus_console/bus_timer/bus_vfs.
 * Returns NULL if no environment device has registered. */
const struct env_ops *bus_env(void);

void env_init(size_t max_entries, size_t max_bytes);

#endif /* CORE_ENV_H */

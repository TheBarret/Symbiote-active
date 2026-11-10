#ifndef CORE_ENV_H
#define CORE_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/version.h>
#include <core/bus.h>

/* Symbiote environment store ("ring collector")
 *
 * A bounded, fixed-capacity table of (namespace, name) -> value entries.
 * Intended as the kernel's read-mostly information surface: system, session,
 * shell state, flags, paths. Consumers discover it through bus_env().
 *
 * Mechanical control points (owned by env):
 *   access_budget  0 = unlimited; N > 0 = entry is reclaimed after N get()s.
 *                  peek() and stat() never consume budget.
 *   ttl_ticks      0 = no expiry; otherwise entry expires ttl ticks after set
 *                  (saturating: it can never wrap into "already expired").
 *   importance     0..ENV_IMPORTANCE_MAX. HIGHER = MORE VALUABLE.
 *
 * Policy flags (mask bits, see mask.h; enforced by env):
 *   READONLY   once set, the entry can never be overwritten, by anyone.
 *              (unset() still removes it; see "unset" below.)
 *   PINNED     never chosen as an eviction victim while live.
 *              Only the OWNER (mask owner of the writer == owner of the entry) may overwrite it.
 *   SECRET     hidden from default listings: list(), list_all(),
 *              and any query whose `care` does not include the SECRET bit.
 *              get/peek/stat by exact name are NOT hidden: env is not an access-control system,
 *              callers that expose env to untrusted code (proxy) must gate it.
 *   CACHEABLE  safe to recompute: evicted before non-cacheable entries of the same importance.
 *
 * Eviction under space pressure, in order of preference:
 *   1. expired entries (even PINNED ones: the TTL already won)
 *   2. lowest importance, but ONLY entries with importance <=
 *      the incoming write's importance are eligible:
 *      a low-importance write can never displace a more valuable entry
 *   3. CACHEABLE before non-cacheable
 *   4. least recently accessed, then name order
 *
 *   PINNED live entries are never eligible;
 *   If room cannot be made WITHOUT evicting something ineligible,
 *   set() fails with ENV_NO_SPACE and nothing at all is evicted or modified: a failed set() has no side effects.
 *
 * Open field (never interpreted by env):
 *   mask           caller-defined classification bits.
 *                  See mask.h for the layout and the difference between subset matching
 *                  (*_by_mask, flags and private bits only) and care/want matching
 *                  (*_match, the correct way to query OWNER and TYPE).
 *
 * Context: thread context only. Never call from an ISR
 * (it allocates, and there is no locking: single-core assumption, same as the rest of the core).
 *
 * Declared settings:
 * ENV_IMPORTANCE_*, ENV_DEFAULT_MAX_*, ENV_NAME_MAX, ENV_VALUE_MAX,
 * ENV_SWEEP_PERIOD_MS, ENV_SWEEP_BATCH live in config.h.
 */

enum env_result {
    ENV_OK                =  0,
    ENV_NOT_FOUND         = -1,   /* no such entry (expired entries read as absent) */
    ENV_READONLY          = -2,   /* set() refused by policy: entry is READONLY, or PINNED by another owner */
    /* -3 reserved (unused: ENV_ACCESS_EXHAUSTED) */
    ENV_NO_SPACE          = -4,   /* set() could not make room (or the heap is exhausted) */
    ENV_INVALID_ARGS      = -5,   /* NULL args, empty name, oversized value, bad mask query */
    ENV_NO_DEVICE         = -6,   /* bus_env() returned NULL / device destroyed */
    ENV_INTERNAL          = -7,   /* invariant violation; caller should log */
    ENV_BUFFER_TOO_SMALL  = -8,   /* get/peek buffer too small; *len_out holds the needed length */
};

/* Per-entry metadata. Read-only snapshot; reading it has no side effects. */
struct env_stat {
    uint32_t value_len;           /* bytes, excluding NUL */
    uint32_t access_remaining;    /* 0 = unlimited */
    bool     has_ttl;
    uint64_t ttl_remaining;       /* ticks; only meaningful when has_ttl */
    uint64_t last_access;         /* ticks */
    uint8_t  importance;
    uint32_t mask;
};

/* Device-level snapshot. */
struct env_info {
    size_t   capacity;            /* slots */
    size_t   count;               /* live entries (may include expired ones not yet swept) */
    size_t   used_bytes;          /* payload bytes: ns + name + value */
    size_t   max_bytes;
    bool     timer_attached;
    uint64_t n_set, n_get, n_peek, n_unset;
    uint64_t n_reclaim_access, n_reclaim_time, n_reclaim_space, n_reclaim_explicit;
    uint64_t n_sweeps;
};

struct env_ops {
    /* Insert or replace. A failed set() leaves the store exactly as it was:
     * the existing entry is untouched and nothing is evicted. */
    int (*set)(const char *ns, const char *name, const char *value,
               uint32_t access_budget, uint64_t ttl_ticks,
               uint8_t importance, uint32_t mask);

    /* Copy the value into buf (NUL-terminated, buf_max includes the NUL).
     * *len_out (optional) receives the value length. If buf is too small,
     * returns ENV_BUFFER_TOO_SMALL, *len_out holds the needed length, and the access budget is NOT consumed.
     * get() consumes one access; peek() does not. */
    int (*get)(const char *ns, const char *name,
               char *buf, size_t buf_max, size_t *len_out);
    int (*peek)(const char *ns, const char *name,
                char *buf, size_t buf_max, size_t *len_out);

    /* Remove an entry. NOT gated by READONLY/PINNED (env cannot see who is calling);
     * callers that face untrusted code must gate it themselves. */
    int (*unset)(const char *ns, const char *name);
    int (*is_exist)(const char *ns, const char *name);

    /* Metadata for one entry. No value, no budget consumed, last_access untouched. */
    int (*stat)(const char *ns, const char *name, struct env_stat *out);

    /* Device counters and capacity. */
    int (*info)(struct env_info *out);

    /* Enumeration by namespace. *iter must be 0 on the first call.
     * Walks slot order, so inserts/removals between calls may cause skips or repeats.
     * Names longer than name_max are truncated. ENV_NOT_FOUND = end, where-as SECRET entries are skipped. */
    int (*list)(const char *ns, size_t *iter,
                char *name_out, size_t name_max);

    /* Enumerate every live, non-SECRET entry across all namespaces (diagnostics, dumps). Same iteration and truncation rules as list(). */
    int (*list_all)(size_t *iter,
                    char *ns_out, size_t ns_max,
                    char *name_out, size_t name_max);

    /* SUBSET match: entries whose mask contains ALL bits of query_mask.
     * Correct for FLAGS and PRIVATE bits only; do NOT use it for OWNER/TYPE (owner 1 is a bit-subset of owner 3).
     * query_mask == 0 is rejected (ENV_INVALID_ARGS). SECRET entries are visible only if query_mask itself includes ENV_FLAG_SECRET. */
    int (*list_by_mask)(uint32_t query_mask, size_t *iter,
                        char *ns_out, size_t ns_max,
                        char *name_out, size_t name_max);

    /* Number of live entries matching query_mask (subset match, as above). */
    int (*count_by_mask)(uint32_t query_mask, size_t *count_out);

    int (*drop_namespace)(const char *ns);

    /* EXACT match: entries where (mask & care) == want.
     * Use the ENV_Q_* helpers from mask.h. care == 0 and want & ~care are rejected (ENV_INVALID_ARGS).
     * SECRET entries are visible only if `care` includes ENV_FLAG_SECRET (want selects secret-only or non-secret-only). */
    int (*list_match)(uint32_t care, uint32_t want, size_t *iter,
                      char *ns_out, size_t ns_max,
                      char *name_out, size_t name_max);
    int (*count_match)(uint32_t care, uint32_t want, size_t *count_out);
};

const struct env_ops *bus_env(void);

/* Idempotent guard: a second call is ignored with a warning. */
void env_init(size_t max_entries, size_t max_bytes);
void env_destroy(void);

#endif /* CORE_ENV_H */

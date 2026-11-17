/* isa.h */
#ifndef CORE_ISA_H
#define CORE_ISA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* codebase: 0.2.0 */

/* Classical 8-bit isa
 *
 * A tick-driven phase sequencer. Each bus cycle spans ISA_PHASE_TICKS ticks;
 * timer_ticks() % ISA_PHASE_TICKS selects the active phase.
 * Peripherals are participants: they respond to phases, one cycle at a time.
 * Super-loop contract: isa_step() is called once per main-loop iteration,
 * in thread context, one phase transition per call (No ISR involvement; no blocking).
 */

/* Timing */

/* Ticks per bus cycle. phase = timer_ticks() % ISA_PHASE_TICKS.
 * Change with care: this is the ISA's clock divider. */
#define ISA_PHASE_TICKS 3u

/* Fixed-capacity transaction queue. Clients push; the ISA pops one per cycle. */
#define ISA_QUEUE_CAP 8u

/* Transaction */

enum isa_dir {
    ISA_READ  = 0,
    ISA_WRITE = 1,
};

enum isa_width {
    ISA_W8  = 1,
    ISA_W16 = 2,
    ISA_W32 = 4,   /* only if peripheral advertises support */
};

enum isa_kind {
    ISA_TXN_MEM = 0,   /* peripheral addresses slot as memory (A) */
    ISA_TXN_SYM = 1,   /* peripheral does keyed symbol lookup (B) */
    /* (A') uses ISA_TXN_SYM semantics with slot addressing; see bridge */
};

enum isa_status {
    ISA_OK              = 0,
    ISA_ERR             = -1,
    ISA_ERR_WIDTH       = -2,   /* peripheral cannot serve this width */
    ISA_ERR_TYPE        = -3,   /* type/width mismatch on a symbol op */
    ISA_ERR_FULL        = -4,   /* peripheral's store returned FULL */
    ISA_ERR_ANSWER      = -5,  /* contract violation: peripheral failed to answer */
};

/* Invoked when a transaction reaches ACK.
 * `txn` is the completed transaction (data filled in for reads, status set).
 * `ctx` is the per-txn context the submitter provided. Runs in thread context (inside isa_step). */
struct isa_txn;
typedef void (*isa_done_fn)(const struct isa_txn *txn, void *ctx);

struct isa_txn {
    uint8_t      kind;
    uint8_t      dir;
    uint8_t      width;
    int8_t       status;
    uint32_t     addr;
    uint32_t     data;

    isa_done_fn  on_done;   /* NULL = fire and forget */
    void        *ctx;       /* passed to on_done */
};

/* Peripherals */

/* A peripheral responds to bus transactions, the ISA knows nothing device-specific.
 * A peripheral that cannot complete a phase before returning is a contract violation (ISA_ERR_ANSWER on the txn).
 *
 * read:  fill *out with `width` bytes' worth of the peripheral's value at `slot`.
 * Return ISA_OK, or an ISA_ERR_* code.
 * write: store `in` (interpreted per `width`) at `slot`. Same return contract.
 */
struct isa_periph {
    const char *name;
    int (*read )(void *ctx, uint32_t slot, uint8_t width, uint32_t *out);
    int (*write)(void *ctx, uint32_t slot, uint8_t width, uint32_t  in);
    void *ctx;
};

/* Attach the single peripheral the ISA fronts. Must be called before isa_start().
 * Returns true on success, false if NULL or already attached. */
bool isa_attach(const struct isa_periph *periph);

/* Lifecycle */

/* Initialize the ISA state. Safe to call once; double-call is a warn-no-op. */
void isa_init(void);

/* Release the ISA's resources (does NOT detach the peripheral). */
void isa_destroy(void);

/* True when the queue and current transaction are both empty. */
bool isa_idle(void);

/* Transactions */

/* Push a transaction onto the queue. Returns true if queued, false if full.
 * The ISA will start the transaction at the next cycle boundary (next time the phase wraps to ADDR). */
bool isa_submit(struct isa_txn txn);

/* Stepping */

/* Advance the ISA by at most one phase, if a phase is due, called from the super loop; thread context only. */
void isa_step(void);

/* Introspection */

struct isa_stats {
    uint64_t cycles_run;      /* completed bus cycles */
    uint64_t phases_skipped;  /* phase boundaries the super loop missed */
    uint64_t txns_queued;     /* total submitted */
    uint64_t txns_completed;  /* total completed (ISA_OK or error) */
    uint64_t txns_errors;     /* completed with status != ISA_OK */
    uint32_t queue_depth;     /* current queue occupancy */
    bool     in_cycle;        /* a transaction is mid-cycle */
};
struct isa_stats isa_get_stats(void);

#endif /* CORE_ISA_H */

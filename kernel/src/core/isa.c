#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lib/mem.h>
#include <lib/string.h>
#include <core/dev/timer.h>
#include <core/isa.h>
#include <core/panic.h>
#include <core/klog.h>

/* codebase: 0.2.0 */

/* Bus phases: ADDR -> DATA -> ACK */
enum isa_phase {
    ISA_PHASE_ADDR = 0,
    ISA_PHASE_DATA = 1,
    ISA_PHASE_ACK  = 2,
};

struct isa_state {
    const struct isa_periph *periph;

    /* Transaction queue: simple fixed ring. Head is the next to run. */
    struct isa_txn queue[ISA_QUEUE_CAP];
    uint32_t       q_head;      /* index of next-to-pop */
    uint32_t       q_tail;      /* index of next-to-push */
    uint32_t       q_count;

    /* Current transaction (mid-cycle), or NULL. */
    struct isa_txn cur;
    bool           in_cycle;

    /* Pacing. */
    uint64_t last_tick;         /* tick at last isa_step() */
    uint8_t  last_phase;        /* phase value at last isa_step() */

    /* Stats. */
    uint64_t cycles_run;
    uint64_t phases_skipped;
    uint64_t txns_queued;
    uint64_t txns_completed;
    uint64_t txns_errors;
};

static struct isa_state g_isa;
static bool g_isa_initialized;

/* queue */

static bool q_push(const struct isa_txn *t) {
    if (g_isa.q_count >= ISA_QUEUE_CAP) return false;
    g_isa.queue[g_isa.q_tail] = *t;
    g_isa.q_tail = (g_isa.q_tail + 1) % ISA_QUEUE_CAP;
    g_isa.q_count++;
    return true;
}

static bool q_pop(struct isa_txn *out) {
    if (g_isa.q_count == 0) return false;
    *out = g_isa.queue[g_isa.q_head];
    g_isa.q_head = (g_isa.q_head + 1) % ISA_QUEUE_CAP;
    g_isa.q_count--;
    return true;
}

/* phases */

static int phase_addr(struct isa_txn *t) {
    /* Address phase: validate the peripheral can serve the request, nothing to write yet;
     * this phase exists so a peripheral can refuse early (bad width) without the DATA phase having to unwind. */
    if (g_isa.periph == NULL) {
        t->status = ISA_ERR_ANSWER;
        return ISA_ERR_ANSWER;
    }
    return ISA_OK;
}

static int phase_data(struct isa_txn *t) {
    /* Data phase: perform the actual read/write via the peripheral. */
    if (t->dir == ISA_WRITE) {
        int rc = g_isa.periph->write(g_isa.periph->ctx, t->addr, t->width, t->data);
        if (rc != ISA_OK) { t->status = (int8_t)rc; return rc; }
    } else {
        uint32_t out = 0;
        int rc = g_isa.periph->read(g_isa.periph->ctx, t->addr, t->width, &out);
        if (rc != ISA_OK) { t->status = (int8_t)rc; return rc; }
        /* Mask to the declared width: the peripheral may return more. */
        uint32_t mask = (t->width == 1) ? 0xFFu
                      : (t->width == 2) ? 0xFFFFu
                      : 0xFFFFFFFFu;
        t->data = out & mask;
    }
    t->status = ISA_OK;
    return ISA_OK;
}

static int phase_ack(struct isa_txn *t) {
    g_isa.cycles_run++;
    g_isa.txns_completed++;
    if (t->status != ISA_OK) g_isa.txns_errors++;
    if (t->on_done != NULL)
        t->on_done(t, t->ctx);
    return ISA_OK;
}

/* Run one phase of the current transaction, given the phase index.
 * Returns true if the transaction completed during this phase. */
static bool run_phase(struct isa_txn *t, uint8_t phase) {
    int rc = ISA_OK;
    switch (phase) {
    case ISA_PHASE_ADDR: rc = phase_addr(t); break;
    case ISA_PHASE_DATA: rc = phase_data(t); break;
    case ISA_PHASE_ACK:  rc = phase_ack(t);  return true;
    default: t->status = ISA_ERR_ANSWER; return true;
    }
    if (rc != ISA_OK) t->status = (int8_t)rc;
    return false;
}

/* public API */

bool isa_attach(const struct isa_periph *periph) {
    if (periph == NULL) return false;
    if (g_isa.periph != NULL) return false;
    g_isa.periph = periph;
    return true;
}

void isa_init(void) {
    if (g_isa_initialized) {
        klog_warning("isa_init() called twice; ignoring\n");
        return;
    }
    memset(&g_isa, 0, sizeof g_isa);
    g_isa_initialized = true;
    klog("→ isa_init() queue=%u, phase_ticks=%u\n", ISA_QUEUE_CAP, ISA_PHASE_TICKS);
}

void isa_destroy(void) {
    if (!g_isa_initialized) return;
    memset(&g_isa, 0, sizeof g_isa);
    g_isa_initialized = false;
}

bool isa_submit(struct isa_txn txn) {
    if (!g_isa_initialized) return false;
    if (!q_push(&txn)) return false;
    g_isa.txns_queued++;
    return true;
}

bool isa_idle(void) {
    return g_isa.q_count == 0 && !g_isa.in_cycle;
}

void isa_step(void) {
    if (!g_isa_initialized) return;

    uint64_t now = timer_ticks();
    uint8_t  phase = (uint8_t)(now % ISA_PHASE_TICKS);

    /* Nothing due: same phase as last call. */
    if (now == g_isa.last_tick) return;

    /* Count skips: if the tick count advanced by more than one, we missed
     * (gap - 1) phase boundaries. Hardware moves on; we observe the miss. */
    uint64_t gap = now - g_isa.last_tick;
    if (gap > 1) g_isa.phases_skipped += (gap - 1);

    /* Pull a new transaction if the phase is ADDR and nothing is in flight. */
    if (!g_isa.in_cycle && phase == ISA_PHASE_ADDR) {
        if (q_pop(&g_isa.cur)) g_isa.in_cycle = true;
    }

    /* If a transaction is in flight, run its phase for this tick. */
    if (g_isa.in_cycle) {
        bool completed = run_phase(&g_isa.cur, phase);
        if (completed) {
            g_isa.in_cycle = false;
            /* completion counters were updated in phase_ack */
        }
    }

    g_isa.last_tick  = now;
    g_isa.last_phase = phase;
}

struct isa_stats isa_get_stats(void) {
    struct isa_stats s = {
        .cycles_run      = g_isa.cycles_run,
        .phases_skipped  = g_isa.phases_skipped,
        .txns_queued     = g_isa.txns_queued,
        .txns_completed  = g_isa.txns_completed,
        .txns_errors     = g_isa.txns_errors,
        .queue_depth     = g_isa.q_count,
        .in_cycle        = g_isa.in_cycle,
    };
    return s;
}

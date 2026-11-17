#include <stdbool.h>
#include <stdint.h>

#include <core/dev/console.h>
#include <core/dev/timer.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/isa.h>
#include <ext/tools/isatest.h>

/* codebase: 0.2.0 */

/* ISA + console peripheral smoke test.
 *
 * Fully asynchronous: the command submits writes and returns; a completion
 * callback fires the read batch once writes ACK; a final completion callback
 * reports. The super loop drives isa_step(); the test never blocks.
 */

struct isatest_state {
    char       wrote[4];
    char       read[4];
    uint8_t    write_done;
    uint8_t    read_done;
    uint8_t    errors;
    uint32_t   read_base;
    struct isa_stats s0;
};

static struct isatest_state g_t;

static uint32_t probe_newest_offset(void);
static void submit_read_batch(void);

static void on_write_done(const struct isa_txn *t, void *ctx) {
    (void)ctx;
    if (t->status != ISA_OK) g_t.errors++;
    g_t.write_done++;

    /* When all three writes ACK, chain into the read batch. */
    if (g_t.write_done == 3) {
        if (g_t.errors > 0) {
            kprintf("isatest: FAIL write error(s)\n");
            return;
        }
        /* The log has grown (the bytes we just wrote are in it).
         * Find the newest retained offset and read the last three bytes. */
        uint32_t newest = probe_newest_offset();
        if (newest == UINT32_MAX || newest < 2) {
            kprintf("isatest: FAIL could not locate newest offset (%u)\n", newest);
            return;
        }
        g_t.read_base = newest - 2;
        submit_read_batch();
    }
}

static void on_read_done(const struct isa_txn *t, void *ctx) {
    uint8_t idx = (uint8_t)(uintptr_t)ctx;
    if (t->status != ISA_OK) {
        g_t.errors++;
    } else {
        g_t.read[idx] = (char)(t->data & 0xFFu);
    }
    g_t.read_done++;

    if (g_t.read_done == 3) {
        g_t.read[3] = '\0';
        struct isa_stats s1 = isa_get_stats();

        kprintf("isatest: wrote \"%c%c%c\", read \"%s\" from offset %u\n",
                g_t.wrote[0], g_t.wrote[1], g_t.wrote[2],
                g_t.read, g_t.read_base);
        kprintf("isatest: cycles +%llu, txns +%llu, errors +%llu, skipped +%llu\n",
                (unsigned long long)(s1.cycles_run     - g_t.s0.cycles_run),
                (unsigned long long)(s1.txns_completed - g_t.s0.txns_completed),
                (unsigned long long)(s1.txns_errors    - g_t.s0.txns_errors),
                (unsigned long long)(s1.phases_skipped - g_t.s0.phases_skipped));

        if (g_t.errors > 0) {
            kprintf("isatest: FAIL %u error(s)\n", g_t.errors);
            return;
        }
        if (g_t.read[0] == 'I' && g_t.read[1] == 'S' && g_t.read[2] == 'A')
            kprintf("isatest: PASS\n");
        else
            kprintf("isatest: FAIL readback mismatch\n");
    }
}

/* Direct peripheral read: walks the retained log to find the newest offset.
 * Uses the peripheral, not the bus, so no transactions are involved. */
static uint32_t probe_newest_offset(void) {
    const struct isa_periph *p = get_console();
    if (p == NULL) return UINT32_MAX;

    uint32_t v = 0;
    if (p->read(p->ctx, 0, ISA_W8, &v) != ISA_OK)
        return UINT32_MAX;

    uint32_t last = 0;
    for (uint32_t off = 1; off < 65536; off++) {
        if (p->read(p->ctx, off, ISA_W8, &v) != ISA_OK) break;
        last = off;
    }
    return last;
}

static void submit_read_batch(void) {
    for (uint8_t i = 0; i < 3; i++) {
        struct isa_txn t = {
            .kind    = ISA_TXN_SYM,
            .dir     = ISA_READ,
            .width   = ISA_W8,
            .addr    = g_t.read_base + i,
            .data    = 0,
            .on_done = on_read_done,
            .ctx     = (void *)(uintptr_t)i,
        };
        if (!isa_submit(t)) {
            kprintf("isatest: FAIL submit read #%u (queue full)\n", i);
            return;
        }
    }
}

int cmd_isatest_fn(const struct cmd_args *a) {
    (void)a;

    if (get_console() == NULL) {
        kprintf("isatest: FAIL: no console peripheral\n");
        return CMD_ERR;
    }

    g_t = (struct isatest_state){0};
    g_t.s0 = isa_get_stats();

    kprintf("isatest: baseline cycles=%llu txns=%llu q=%u\n",
            (unsigned long long)g_t.s0.cycles_run,
            (unsigned long long)g_t.s0.txns_completed,
            g_t.s0.queue_depth);

    const char *payload = "ISA";
    for (uint8_t i = 0; i < 3; i++) {
        struct isa_txn t = {
            .kind    = ISA_TXN_SYM,
            .dir     = ISA_WRITE,
            .width   = ISA_W8,
            .addr    = 0,
            .data    = (uint32_t)(uint8_t)payload[i],
            .on_done = on_write_done,
            .ctx     = NULL,
        };
        if (!isa_submit(t)) {
            kprintf("isatest: FAIL submit write #%u (queue full)\n", i);
            return CMD_ERR;
        }
        g_t.wrote[i] = payload[i];
    }

    /* Return immediately. The super loop drives isa_step(); completion
     * callbacks fire the read batch and the report. */
    kprintf("isatest: 3 writes queued; will report on completion\n");
    return CMD_OK;
}

/* Moved to testers.c
SYM_COMMAND(isatest, "", "Perform ISA + console peripheral test", cmd_isatest_fn);

static int isatest_ext_init(void) {
    return 0;
}

SYM_EXTENSION(isatest_toolkit, isatest_ext_init, EXT_PRIO_APPLET);
 */

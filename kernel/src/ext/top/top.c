#include <stddef.h>
#include <stdint.h>

#include <lib/string.h>

#include <core/cmd.h>
#include <core/tui.h>
#include <core/kprintf.h>
#include <core/klog.h>

#include <core/bus.h>
#include <core/dev/env.h>
#include <core/dev/timer.h>
#include <core/arch/pmm.h>
#include <core/arch/heap.h>
#include <core/arch/isr.h>

/*  top — SYMBIOTE system monitor (TUI)
 *
 *  v1: all panels populated. Snapshot discipline active.
 *
 *  Refresh: `n` ms via timer_after. Coprime with ENV_SWEEP_PERIOD_MS (1000)
 *  and TIMER_PUBLISH_MS (250), so `top`'s drain cannot phase-lock with the
 *  env sweeper or the timer publisher. Draw reads `prev`, never live state,
 *  so each frame is internally consistent and cannot reflect its own draw cost.
 *
 *  Deferred:
 *    - IRQ names       (would require irq_register to take a name; see isr.h)
 *    - device scroll   (device count fits panel today; add v2 if it doesn't)
 */

#define TOP_REFRESH_MS  2600u
#define TOP_IRQ_LINES   16

struct top_snapshot {
    uint64_t timer_ticks;
    uint64_t uptime_ms;
    uint32_t timer_hz;

    struct pmm_stats  pmm;
    struct heap_stats heap;
    struct env_info   env;

    struct irq_info   irq[TOP_IRQ_LINES];

    size_t dev_count;                          /* cached bus_count()       */
    struct bus_device dev[BUS_MAX_DEVICES];    /* copy, not pointer        */
};

struct top_state {
    struct tui_view    view;
    int                timer_id;      /* scheduled refresh; -1 when not scheduled */
    struct top_snapshot now;          /* last captured */
    struct top_snapshot prev;         /* one cycle behind; the panel displays this */
};

/*  snapshot capture  */

static void capture(struct top_snapshot *s) {
    const struct timer_ops *t = bus_timer();
    if (t) {
        s->timer_ticks = t->ticks();
        s->uptime_ms   = t->uptime_ms();
        s->timer_hz    = t->hz();
    }

    s->pmm  = pmm_get_stats();
    s->heap = heap_get_stats();

    const struct env_ops *env = bus_env();
    if (env) { (void)env->info(&s->env); }

    for (int i = 0; i < TOP_IRQ_LINES; i++)
        s->irq[i] = irq_get_info(i);

    s->dev_count = bus_count();
    if (s->dev_count > BUS_MAX_DEVICES)
        s->dev_count = BUS_MAX_DEVICES;
    for (size_t i = 0; i < s->dev_count; i++) {
        const struct bus_device *d = bus_at(i);
        if (d)
            s->dev[i] = *d;
        else
            s->dev[i].name = NULL;
    }
}

/*  layout  */

struct top_layout {
    int w, h;
    struct tui_rect header;
    struct tui_rect footer;

    struct tui_rect mem;
    struct tui_rect env;
    struct tui_rect dev;
    struct tui_rect irq;
    struct tui_rect kbd;
};

/* Columns: left stack {mem, env, dev}, right stack {irq, kbd}.
 * Left column takes 60%, right takes the rest. */
static void top_layout_init(struct top_layout *L, int w, int h) {
    L->w = w;
    L->h = h;

    L->header = (struct tui_rect){ 0, 0,     w, 1 };
    L->footer = (struct tui_rect){ 0, h - 1, w, 1 };

    int body_y = 2;
    int body_h = h - 4;
    if (body_h < 9) body_h = 9;              /* safety for 80x24 minimum */

    int left_w  = (w * 3) / 5;
    int right_x = left_w + 1;
    int right_w = w - right_x;

    /* Left: three stacked panels with 1-row gaps.
     *   mem n1%, env n2%, dev remainder. */
    int mem_h = (body_h * 20) / 100; //n1
    int env_h = (body_h * 25) / 100; //n2
    int dev_h = body_h - mem_h - env_h - 2;  /* bottom padding n-gaps */
    if (mem_h < 4) mem_h = 4;
    if (env_h < 4) env_h = 4;
    if (dev_h < 4) dev_h = body_h - mem_h - env_h - 2;

    L->mem = (struct tui_rect){ 0, body_y,                        left_w, mem_h };
    L->env = (struct tui_rect){ 0, body_y + mem_h + 1,            left_w, env_h };
    L->dev = (struct tui_rect){ 0, body_y + mem_h + env_h + 2,    left_w, dev_h };

    /* Right: irq 55%, kbd remainder */
    int irq_h = (body_h * 55) / 100;
    int kbd_h = body_h - irq_h - 1;
    if (irq_h < 4) irq_h = 4;
    if (kbd_h < 4) kbd_h = body_h - irq_h - 1;

    L->irq = (struct tui_rect){ right_x, body_y,             right_w, irq_h };
    L->kbd = (struct tui_rect){ right_x, body_y + irq_h + 1, right_w, kbd_h };
}

/* Draw a titled panel: box, title on the top border, return interior rect. */
static struct tui_rect panel(struct tui_rect r, const char *title, uint8_t attr) {
    if (r.w < 4 || r.h < 3)
        return (struct tui_rect){ 0, 0, 0, 0 };

    tui_box(r, attr);

    int tx = r.x + 2;
    int avail = r.w - 4;
    for (int i = 0; i < avail; i++)
        tui_put(tx + i, r.y, TUI_H, attr);
    tui_text(tx, r.y, attr, " %s ", title);

    return (struct tui_rect){ r.x + 1, r.y + 1, r.w - 2, r.h - 2 };
}

/*  formatting helpers  */

static void fmt_uptime(uint64_t ms, char *out, size_t cap) {
    uint64_t sec = ms / 1000;
    uint64_t d   = sec / 86400;
    uint64_t h   = (sec % 86400) / 3600;
    uint64_t m   = (sec % 3600) / 60;
    uint64_t s   = sec % 60;
    if (d > 0)
        ksnprintf(out, cap, "%llud %02llu:%02llu:%02llu",
                  (unsigned long long)d, (unsigned long long)h,
                  (unsigned long long)m, (unsigned long long)s);
    else
        ksnprintf(out, cap, "%02llu:%02llu:%02llu",
                  (unsigned long long)h, (unsigned long long)m,
                  (unsigned long long)s);
}

/* mem x KiB -> "N MiB" or "N KiB" */
static void fmt_kib(uint64_t kib, char *out, size_t cap) {
    if (kib >= 1024)
        ksnprintf(out, cap, "%llu.%03llu MiB",
                  (unsigned long long)(kib / 1024),
                  (unsigned long long)((kib % 1024) * 1000 / 1024));
    else
        ksnprintf(out, cap, "%llu KiB", (unsigned long long)kib);
}

/* delta between two u64 counters, safe under wrap */
static uint64_t udelta(uint64_t now, uint64_t prev) {
    return (now >= prev) ? (now - prev) : 0;
}

/*  panel drawing  */

static void draw_header(struct top_layout *L, const struct top_snapshot *s) {
    uint8_t bg = TUI_ATTR(TUI_FG_BLACK, TUI_FG_GREEN);
    tui_fill(L->header, ' ', bg);

    tui_text(1, L->header.y, bg, "Top");

    char up[32];
    fmt_uptime(s->uptime_ms, up, sizeof up);
    tui_text(L->w / 2 - 6, L->header.y, bg, "Uptime: %s", up);

    tui_text(L->w - 14, L->header.y, bg, "Rate: %luHz",
             (unsigned long)s->timer_hz);
}

static void draw_memory(const struct tui_rect r, const struct top_snapshot *s,
                        const struct top_snapshot *p) {
    uint8_t a = TUI_ATTR(TUI_FG_WHITE, TUI_FG_BLACK);
    struct tui_rect in = panel(r, "MEMORY", a);
    if (in.w <= 0) return;

    uint64_t total_kib = (uint64_t)s->pmm.total_frames * 4;
    uint64_t free_kib  = (uint64_t)s->pmm.free_frames  * 4;
    uint64_t used_kib  = total_kib - free_kib;

    uint64_t d_free = udelta(s->pmm.free_frames, p->pmm.free_frames);

    int y = in.y;

    tui_text(in.x + 1, y++, a, "Phys  %llu / %llu KiB used",
             (unsigned long long)used_kib, (unsigned long long)total_kib);
    if (y < in.y + in.h) {
        char f[24];
        fmt_kib(free_kib, f, sizeof f);
        tui_text(in.x + 1, y++, a, "Free  %s  (d: -%llu f)", f,
                 (unsigned long long)d_free);
    }
    if (y < in.y + in.h) {
        tui_text(in.x + 1, y++, a, "Heap  %lu chunks  %lu blocks  %lu free",
                 (unsigned long)s->heap.chunks,
                 (unsigned long)s->heap.blocks,
                 (unsigned long)s->heap.blocks_free);
    }
    if (y < in.y + in.h) {
        tui_text(in.x + 1, y++, a, "      %lu used  %lu free",
                 (unsigned long)s->heap.bytes_used,
                 (unsigned long)s->heap.bytes_free);
    }
    if (y < in.y + in.h) {
        tui_text(in.x + 1, y++, a, "      largest %lu B",
                 (unsigned long)s->heap.largest_free);
    }
}

static void draw_env(const struct tui_rect r, const struct top_snapshot *s,
                     const struct top_snapshot *p) {
    uint8_t a = TUI_ATTR(TUI_FG_WHITE, TUI_FG_BLACK);
    struct tui_rect in = panel(r, "ENVIRONMENT STORE", a);
    if (in.w <= 0) return;

    int y = in.y;

    tui_text(in.x + 1, y++, a, "Slots %lu / %lu    Bytes %lu / %lu",
             (unsigned long)s->env.count,
             (unsigned long)s->env.capacity,
             (unsigned long)s->env.used_bytes,
             (unsigned long)s->env.max_bytes);

    if (y < in.y + in.h) {
        tui_text(in.x + 1, y++, a, "Ops   set %llu  get %llu  peek %llu  unset %llu",
                 (unsigned long long)s->env.n_set,
                 (unsigned long long)s->env.n_get,
                 (unsigned long long)s->env.n_peek,
                 (unsigned long long)s->env.n_unset);
    }
    if (y < in.y + in.h) {
        tui_text(in.x + 1, y++, a, "Recl  acc %llu  ttl %llu  sp %llu  exp %llu",
                 (unsigned long long)s->env.n_reclaim_access,
                 (unsigned long long)s->env.n_reclaim_time,
                 (unsigned long long)s->env.n_reclaim_space,
                 (unsigned long long)s->env.n_reclaim_explicit);
    }
    if (y < in.y + in.h) {
        uint64_t d_sweeps = udelta(s->env.n_sweeps, p->env.n_sweeps);
        tui_text(in.x + 1, y++, a, "Sweeps %llu  (d:%llu)   timer %s",
                 (unsigned long long)s->env.n_sweeps,
                 (unsigned long long)d_sweeps,
                 s->env.timer_attached ? "attached" : "off");
    }
}

/* shortname: class description */
static const char *class_name(enum bus_class c) {
    switch (c) {
        case BUS_CLASS_NONE:     return "none";
        case BUS_CLASS_CONSOLE:  return "IO";
        case BUS_CLASS_TIMER:    return "ISR";
        case BUS_CLASS_ENV:      return "MEMORY";
        case BUS_CLASS_VFS:      return "MEMORY";
        case BUS_CLASS_BUS:      return "IO";
        case BUS_CLASS_VM16:     return "IO";
        case BUS_CLASS_PROXY:    return "IO";
        case BUS_CLASS_KEYBOARD: return "IO";
        case BUS_CLASS_METRIC:   return "IO";
        default:                 return "undefined";
    }
}

static void draw_devices(const struct tui_rect r, const struct top_snapshot *s) {
    uint8_t a = TUI_ATTR(TUI_FG_WHITE, TUI_FG_BLACK);
    struct tui_rect in = panel(r, "BUS DEVICES", a);
    if (in.w <= 0) return;

    int y = in.y;
    int max_rows = in.h;

    tui_text(in.x + 1, y++, a, "%-10s %-8s %-3s %-3s",
             "NAME", "CLASS", "VERSION", "ID");

    for (size_t i = 0; i < s->dev_count && (y - in.y) < max_rows; i++, y++) {
        const struct bus_device *d = &s->dev[i];
        if (d->name == NULL) continue;
        tui_text(in.x + 1, y, a, "%-10.10s %-8.8s %-7u %-3u",
                 d->name,
                 class_name(d->class),
                 (unsigned)d->version,
                 (unsigned)d->owner_id);
    }
}

static void draw_irq(const struct tui_rect r, const struct top_snapshot *s) {
    uint8_t a = TUI_ATTR(TUI_FG_WHITE, TUI_FG_BLACK);
    struct tui_rect in = panel(r, "IRQ TABLE", a);
    if (in.w <= 0) return;

    int y = in.y;
    int max_rows = in.h;

    tui_text(in.x + 1, y++, a, "line   count     spur   unhandled");

    for (int i = 0; i < TOP_IRQ_LINES && (y - in.y) < max_rows; i++, y++) {
        const struct irq_info *ii = &s->irq[i];
        if (!ii->registered && ii->count == 0 && ii->spurious == 0 && ii->unhandled == 0)
            continue;
        tui_text(in.x + 1, y, a, " %2d   %8llu  %5llu  %8llu",
                 i,
                 (unsigned long long)ii->count,
                 (unsigned long long)ii->spurious,
                 (unsigned long long)ii->unhandled);
    }
}

static void draw_footer(struct top_layout *L) {
    uint8_t bg = TUI_ATTR(TUI_FG_BLACK, TUI_FG_GREEN);
    tui_fill(L->footer, ' ', bg);
    tui_text(1, L->footer.y, bg, "q: quit, r: refresh");
    tui_text(L->w - 20, L->footer.y, bg, "System Monitor");
}

/*  view callbacks  */

static void top_draw(struct tui_view *v) {
    struct top_state *s = v->state;

    int w = 0, h = 0;
    tui_size(&w, &h);

    struct top_layout L;
    top_layout_init(&L, w, h);

    tui_clear(TUI_ATTR_DEFAULT);

    /* Draw from `prev`, never from `now` — the frame is one cycle stale,
     * internally consistent, and cannot reflect its own draw cost. */
    draw_header(&L, &s->prev);
    draw_memory(L.mem, &s->prev, &s->prev);
    draw_env(L.env, &s->prev, &s->prev);
    draw_devices(L.dev, &s->prev);
    draw_irq(L.irq, &s->prev);
    draw_footer(&L);
}

static bool top_key(struct tui_view *v, enum kbd_event ev, char c) {
    struct top_state *s = v->state;

    if (ev == KBD_CHAR && (c == 'q' || c == 'Q'))
        return false;
    if (ev == KBD_SPECIAL && c == KBD_KEY_ESC)
        return false;
    if (ev == KBD_CHAR && (c == 'r' || c == 'R')) {
        /* immediate refresh: rotate now into prev and draw */
        s->prev = s->now;
        capture(&s->now);
        s->prev = s->now;
        return true;                     /* tui_run will call draw */
    }
    return true;
}

/*  refresh timer  */

static void top_refresh(void *arg) {
    struct top_state *s = arg;

    if (s->timer_id < 0)
        return;                          /* cancelled */

    s->prev = s->now;
    capture(&s->now);

    /* Draw into the back grid, flush. This is the only place a non-key
     * draw happens; it runs in timer_drain's thread context. */
    top_draw(&s->view);
    tui_flush();

    s->timer_id = timer_after(TOP_REFRESH_MS, top_refresh, s);
}

/*  command entry  */

static int cmd_top_fn(const struct cmd_args *a) {
    (void)a;

    struct top_state state = {0};
    state.timer_id = -1;

    /* prime: capture twice so `now` and `prev` are both valid on first draw */
    capture(&state.now);
    state.prev = state.now;

    state.view = (struct tui_view){
        .draw  = top_draw,
        .key   = top_key,
        .state = &state,
    };

    if (!tui_begin()) {
        kprintf("top: TUI engine unavailable\n");
        return CMD_USAGE;
    }

    /* first draw before the timer starts, so the user sees data immediately */
    top_draw(&state.view);
    tui_flush();

    state.timer_id = timer_after(TOP_REFRESH_MS, top_refresh, &state);

    tui_run(&state.view);

    /* cancel refresh before teardown */
    if (state.timer_id >= 0) {
        timer_cancel(state.timer_id);
        state.timer_id = -1;
    }

    tui_end();
    return CMD_OK;
}

SYM_COMMAND(top, "", "System monitor", cmd_top_fn);

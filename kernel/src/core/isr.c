#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/isr.h>
#include <core/panic.h>
#include <core/pic.h>

_Static_assert(sizeof(struct isr_frame) == 22 * 8, "isr_frame must match isr_common's pushes");

bool isr_has_error_code(int v) {
    return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30;
}

const char *isr_exception_name(int v) {
    static const char *const names[32] = {
        "#DE divide error",             "#DB debug",
        "NMI non-maskable interrupt",   "#BP breakpoint",
        "#OF overflow",                 "#BR bound range exceeded",
        "#UD invalid opcode",           "#NM device not available",
        "#DF double fault",             "coprocessor segment overrun",
        "#TS invalid TSS",              "#NP segment not present",
        "#SS stack-segment fault",      "#GP general protection fault",
        "#PF page fault",               "(reserved 15)",
        "#MF x87 floating-point error", "#AC alignment check",
        "#MC machine check",            "#XM SIMD floating-point error",
        "#VE virtualization exception", "#CP control protection",
        "(reserved 22)", "(reserved 23)", "(reserved 24)", "(reserved 25)",
        "(reserved 26)", "(reserved 27)",
        "#HV hypervisor injection",     "#VC VMM communication",
        "#SX security exception",       "(reserved 31)",
    };
    return (v >= 0 && v < 32) ? names[v] : "(not an exception)";
}

/*  IRQ table - */

static struct {
    irq_handler_t fn;
    void *ctx;
} irq_table[16];

static volatile uint64_t irq_counts[16];
static volatile uint64_t irq_spurious[16];
static volatile uint64_t irq_unhandled[16];

int irq_register(int irq, irq_handler_t fn, void *ctx) {
    if (irq < 0 || irq > 15 || irq == 2 || fn == NULL)
        return -1;
    if (irq_table[irq].fn != NULL)
        return -1;
    irq_table[irq].ctx = ctx;
    irq_table[irq].fn = fn;
    pic_unmask(irq);
    return 0;
}

void irq_unregister(int irq) {
    if (irq < 0 || irq > 15 || irq == 2)
        return;
    pic_mask(irq);
    irq_table[irq].fn = NULL;
    irq_table[irq].ctx = NULL;
}

struct irq_info irq_get_info(int irq) {
    struct irq_info info = {0};
    if (irq < 0 || irq > 15)
        return info;
    info.registered = irq_table[irq].fn != NULL;
    info.count = irq_counts[irq];
    info.spurious = irq_spurious[irq];
    info.unhandled = irq_unhandled[irq];
    return info;
}

/*  the one entry point */

void interrupt_dispatch(struct isr_frame *f) {
    uint64_t v = f->vector;

    if (f->vector == 0xFF)      /* LAPIC spurious: ignore, no EOI */
        return;

    if (v < 32)
        panic_exception(f);                 /* every CPU exception is fatal, and reported in full */

    if (v < PIC_VECTOR_BASE + 16) {
        int irq = (int)(v - PIC_VECTOR_BASE);

        if (pic_irq_is_spurious(irq)) {     /* the right EOI (or none) was already sent */
            irq_spurious[irq]++;
            return;
        }

        irq_counts[irq]++;
        if (irq_table[irq].fn)
            irq_table[irq].fn(irq_table[irq].ctx);
        else
            irq_unhandled[irq]++;           /* masked lines should never land here */

        pic_eoi(irq);
        return;
    }

    panic_exception(f);                     /* vector 48-255: nothing ever installs these */
}

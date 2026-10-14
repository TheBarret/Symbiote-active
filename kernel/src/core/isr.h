#ifndef CORE_ISR_H
#define CORE_ISR_H

#include <stdbool.h>
#include <stdint.h>

/* Everything the CPU and isr_stubs.S saved when a vector fired.
 * Field order is the exact reverse of the push order in isr_common, lowest address first:
 * change one side and you MUST change the other (the static assert in isr.c
 * only catches a wrong size, not a swapped pair). */
struct isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;        /* pushed by the stub */
    uint64_t error;         /* pushed by the CPU, or 0 pushed by the stub */
    uint64_t rip, cs, rflags, rsp, ss;     /* pushed by the CPU */
};

/* Called from isr_stubs.S for every vector. */
void interrupt_dispatch(struct isr_frame *f);

/* True for the vectors where the CPU pushes a real error code. */
bool isr_has_error_code(int vector);

/* "#PF page fault" style name for vectors 0-31. */
const char *isr_exception_name(int vector);

/*  hardware interrupts (IRQ 0-15 = vectors 0x20-0x2f)  */

/* RULE: a handler runs with interrupts off, inside whatever was interrupted.
 * It must NOT call kprintf/klog/kmalloc/pmm/vmm (none of them are reentrant).
 * Count, set a flag, or push to a ring buffer; let thread context do the rest. */
typedef void (*irq_handler_t)(void *ctx);

/* Attach a handler to an IRQ line and unmask it.
 * Returns 0 on success, -1 if the line is out of range, is the cascade (IRQ2), or already has a handler.
 * An extension should fail its init when this returns non-zero. */
int irq_register(int irq, irq_handler_t fn, void *ctx);

/* Mask the line and detach its handler. */
void irq_unregister(int irq);

struct irq_info {
    bool     registered;
    uint64_t count;         /* interrupts delivered to the handler */
    uint64_t spurious;      /* spurious IRQ7/IRQ15 events ignored */
    uint64_t unhandled;     /* arrived with no handler (should stay 0) */
};
struct irq_info irq_get_info(int irq);

#endif

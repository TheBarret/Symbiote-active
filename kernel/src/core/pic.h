#ifndef CORE_PIC_H
#define CORE_PIC_H

#include <stdbool.h>
#include "core/version.h"

/* 8259 PIC pair. This is the seam an APIC would replace:
 * callers only use mask / unmask / eoi, so swapping controllers changes pic.c and nothing else. */

// Moved to ref: core/version.h
//#define PIC_VECTOR_BASE 0x20    /* IRQ0 -> vector 0x20 ... IRQ15 -> vector 0x2f */

/* Remap the PIC away from the exception vectors and mask every line.
 * Call this before the first cpu_sti(). */
void pic_init(void);

void pic_mask(int irq);
void pic_unmask(int irq);   /* unmasking a slave line (8-15) also opens the IRQ2 cascade */

/* End-of-interrupt. Slave lines need both controllers acknowledged.
 *
 * Do NOT call this after pic_irq_is_spurious() returns true.
 * That function already sent the correct (or no) EOI, and a second one will desynchronise the in-service register. */
void pic_eoi(int irq);

/* IRQ7 and IRQ15 can fire with nothing behind them.
 * Returns true if this one is spurious; the right (or no) EOI has already been sent, so the caller must NOT call pic_eoi(). */
bool pic_irq_is_spurious(int irq);

#endif

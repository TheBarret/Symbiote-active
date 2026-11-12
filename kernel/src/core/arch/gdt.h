#ifndef CORE_GDT_H
#define CORE_GDT_H

#include <stdint.h>

/* Global Descriptor Table + Task State Segment.
 *
 * Symbiote runs everything in ring 0, so there are no user segments:
 *   0x00  Null descriptor
 *   0x08  Kernel code (64-bit long mode)
 *   0x10  Kernel data
 *   0x18  TSS descriptor (16-byte system descriptor occupying TWO table slots: 0x18 and 0x20)
 *
 * The TSS exists primarily for its Interrupt Stack Table (IST).
 * An IDT vector assigned to an IST slot executes on a dedicated, known-good stack,
 * ensuring that critical exceptions like Double Faults (#DF) or NMIs do not trigger
 * triple faults due to a corrupted or exhausted stack.
 */

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_TSS   0x18

/* IST slot numbers (1-based, matching the IDT gate ist field). */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

/* Initialize GDT, reload segment registers via far return, and load task register. */
void gdt_init(void);

#endif

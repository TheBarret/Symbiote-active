#ifndef CORE_GDT_H
#define CORE_GDT_H

#include <stdint.h>

/* Global Descriptor Table + Task State Segment.
 *
 * Symbiote runs everything in ring 0, so there are no user segments:
 *   0x00  null
 *   0x08  kernel code (64-bit)
 *   0x10  kernel data
 *   0x18  TSS (a 16-byte descriptor: it occupies TWO table slots)
 *
 * The TSS exists for its Interrupt Stack Table (IST).
 * A vector whose IDT gate names an IST slot always runs on that known-good stack,
 * so a double fault caused by a wrecked kernel stack still produces a report instead of a triple fault.
 * Add ring-3 segments only when there is a ring 3. */

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_TSS   0x18

/* IST slot numbers (1-based, as the IDT gate field wants them). */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

/* Load the GDT, reload CS/DS/ES/SS, and load the task register.
 * Needs no heap or PMM: every table and stack here is static. */
void gdt_init(void);

#endif

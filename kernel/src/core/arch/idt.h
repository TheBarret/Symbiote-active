#ifndef CORE_IDT_H
#define CORE_IDT_H

/* Interrupt Descriptor Table: all 256 vectors.
 * Every vector is populated with an explicit handler stub so unexpected
 * interrupts or exceptions route to the dispatch handler instead of causing
 * unhandled faults or triple faults. Requires gdt_init() to be run first.
 */
void idt_init(void);

#endif

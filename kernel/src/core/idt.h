#ifndef CORE_IDT_H
#define CORE_IDT_H

/* Interrupt Descriptor Table: all 256 vectors, installed once at boot.
 * Every vector gets a real gate, so a stray or unexpected interrupt lands in interrupt_dispatch(),
 * and is reported by number instead of becoming a confusing #GP or a triple fault.
 * Needs gdt_init() first (it names the code selector and the IST stacks). Needs no heap. */
void idt_init(void);

#endif

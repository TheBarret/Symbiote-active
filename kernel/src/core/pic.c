#include "klog.h"
#include <stdbool.h>
#include <stdint.h>
#include <core/cpu.h>
#include <core/pic.h>

#include <core/vmm.h>
#include <core/klog.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI        0x20
#define PIC_READ_ISR   0x0B     /* OCW3: next read of the command port returns the In-Service Register */

#define ICW1_INIT      0x10
#define ICW1_ICW4      0x01
#define ICW4_8086      0x01


#define LAPIC_VA     (VMM_DYNAMIC_BASE + 0x200000ull)   /* clear of vmtest's page */
#define LAPIC_SVR    (0x0F0 / 4)
#define LAPIC_LINT0  (0x350 / 4)

static uint16_t irq_mask_bits = 0xFFFF;     /* bit n set = IRQ n masked */

/* The 8259 is slow; a write to the unused port 0x80 gives it time to settle. */
static inline void io_wait(void) { outb(0x80, 0); }

static void write_masks(void) {
    outb(PIC1_DATA, (uint8_t)(irq_mask_bits & 0xFF));
    outb(PIC2_DATA, (uint8_t)(irq_mask_bits >> 8));
}

static void lapic_virtual_wire(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));   /* IA32_APIC_BASE */
    if (!(lo & (1u << 11)) || (lo & (1u << 10)))
        return;     /* APIC off: the PIC already reaches the CPU. x2APIC: registers are MSRs, not handled yet */
    uint64_t phys = (((uint64_t)hi << 32) | lo) & 0x000ffffffffff000ull;
    if (!vmm_map(LAPIC_VA, phys, VMM_WRITE | VMM_NX | VMM_NOCACHE)) {
        /* If the map fails, IRQs never arrive and the only clue would be the absence of activity, warn user. */
        klog_warning("→ lapic_virtual_wire(): map failed, PIC-direct only\n");
        return;
    }
    volatile uint32_t *r = (volatile uint32_t *)LAPIC_VA;
    klog("→ lapic_virtual_wire(): lint0=%08x svr=%08x\n", r[LAPIC_LINT0], r[LAPIC_SVR]);
    r[LAPIC_SVR]   = (r[LAPIC_SVR] & ~0xFFu) | 0x100 | 0xFF;   /* software-enable, spurious vector 0xFF */
    r[LAPIC_LINT0] = 0x700;                                    /* ExtINT, unmasked */
}

void pic_init(void) {
    /* ICW1: start initialisation, expect ICW4. */
    klog("→ pic_init() initializing...\n");
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    /* ICW2: vector offsets. Without this IRQ0-7 land on vectors 8-15,
     * on top of the CPU exceptions (IRQ0 would look like a double fault). */
    outb(PIC1_DATA, PIC_VECTOR_BASE);      io_wait();
    outb(PIC2_DATA, PIC_VECTOR_BASE + 8);  io_wait();
    /* ICW3: the slave hangs off the master's IRQ2. */
    outb(PIC1_DATA, 1 << 2);               io_wait();
    outb(PIC2_DATA, 2);                    io_wait();
    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, ICW4_8086);            io_wait();
    outb(PIC2_DATA, ICW4_8086);            io_wait();

    /* Everything masked: nothing fires until a handler asks for its line. */
    irq_mask_bits = 0xFFFF;
    write_masks();
    lapic_virtual_wire();
}

/* Pic_mask and pic_unmask now bracket their read-modify-write of irq_mask_bits and the two write_masks() outb's in an IRQ-save region.
 * Without it a nested (or, later, SMP) caller can lose a bit update or interleave the two outb's, leaving the shadow and the hardware out of step.
 * Single-core boot does not hit this today; the cost of the fix is two instructions and the cost of the bug later is a mystery. */

void pic_mask(int irq) {
    if (irq < 0 || irq > 15)
        return;
    uint64_t flags = cpu_irq_save();
    irq_mask_bits |= (uint16_t)(1u << irq);
    write_masks();
    cpu_irq_restore(flags);
}

void pic_unmask(int irq) {
    if (irq < 0 || irq > 15)
        return;
    uint64_t flags = cpu_irq_save();
    irq_mask_bits &= (uint16_t)~(1u << irq);
    if (irq >= 8)
        irq_mask_bits &= (uint16_t)~(1u << 2);      /* open the cascade too */
    write_masks();
    cpu_irq_restore(flags);
}

/* Bounds check added. pic_mask and pic_unmask already guard the same range; pic_eoi was the odd one out.
 * Calling it with an out-of-range irq would send an EOI to one or both controllers with no corresponding interrupt,
 * which can unmask a pending line early. */
void pic_eoi(int irq) {
    if (irq < 0 || irq > 15)
        return;
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

bool pic_irq_is_spurious(int irq) {
    if (irq == 7) {
        outb(PIC1_CMD, PIC_READ_ISR);
        if (!(inb(PIC1_CMD) & 0x80))
            return true;                /* spurious master IRQ7: send NO EOI */
    } else if (irq == 15) {
        outb(PIC2_CMD, PIC_READ_ISR);
        if (!(inb(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, PIC_EOI);    /* the master saw the cascade line, so it needs one */
            return true;                /* but the slave gets none */
        }
    }
    return false;
}

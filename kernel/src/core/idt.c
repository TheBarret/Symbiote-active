#include <stdint.h>
#include <core/cpu.h>
#include <core/gdt.h>
#include <core/idt.h>

/* One 64-bit interrupt gate. 16 bytes; 256 of them are 4 KiB. */
struct idt_gate {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;           /* low 3 bits: IST slot, 0 = use the current stack */
    uint8_t  type_attr;     /* 0x8E = present, DPL 0, interrupt gate (clears IF) */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_gate) == 16, "IDT gate must be 16 bytes");

static struct idt_gate idt[256] __attribute__((aligned(16)));

/* Addresses of the 256 stubs in isr_stubs.S. */
extern const uint64_t isr_stub_table[256];

static void set_gate(int vector, uint64_t handler, uint8_t ist) {
    idt[vector] = (struct idt_gate){
        .offset_lo  = (uint16_t)(handler & 0xFFFF),
        .selector   = GDT_KCODE,
        .ist        = ist,
        .type_attr  = 0x8E,
        .offset_mid = (uint16_t)((handler >> 16) & 0xFFFF),
        .offset_hi  = (uint32_t)(handler >> 32),
        .reserved   = 0,
    };
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) {
        uint8_t ist = 0;
        if (v == 8)
            ist = IST_DOUBLE_FAULT;     /* a wrecked stack must not take the report down with it */
        else if (v == 2)
            ist = IST_NMI;              /* an NMI can land at any instruction, on any stack */
        set_gate(v, isr_stub_table[v], ist);
    }

    struct dt_ptr ptr = { .limit = sizeof idt - 1, .base = (uint64_t)idt };
    cpu_lidt(&ptr);
}

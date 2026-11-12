#include <stdint.h>
#include <core/arch/cpu.h>
#include <core/arch/gdt.h>
#include <core/arch/idt.h>

/* 64-bit Interrupt Gate descriptor (16 bytes). */
struct idt_gate {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;          /* Bits 0-2: IST index (0 = current stack, 1-7 = dedicated IST stack) */
    uint8_t  type_attr;    /* 0x8E = Present, DPL 0, 64-bit Interrupt Gate (automatically clears IF) */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_gate) == 16, "IDT gate must be 16 bytes");

static struct idt_gate idt[256] __attribute__((aligned(16)));

/* External table of 256 ISR stub entry points defined in assembly. */
extern const uint64_t isr_stub_table[256];

static void set_gate(int vector, uint64_t handler, uint8_t ist) {
    idt[vector] = (struct idt_gate){
        .offset_lo  = (uint16_t)(handler & 0xFFFF),
        .selector   = GDT_KCODE,
        .ist        = ist & 0x7, /* Restrict IST to valid 3-bit range (0-7) */
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
            ist = IST_DOUBLE_FAULT; /* Protect against stack corruption during #DF */
        else if (v == 2)
            ist = IST_NMI;          /* NMI can arrive asynchronously on any instruction */
        set_gate(v, isr_stub_table[v], ist);
    }

    struct dt_ptr ptr = { .limit = sizeof(idt) - 1, .base = (uint64_t)idt };
    cpu_lidt(&ptr);
}

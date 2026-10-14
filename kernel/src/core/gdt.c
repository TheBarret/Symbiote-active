#include <stdint.h>
#include <core/cpu.h>
#include <core/gdt.h>

/* 64-bit Task State Segment. Only the IST and RSP0 fields matter here. */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];        /* stacks for privilege changes (unused: no ring 3) */
    uint64_t reserved1;
    uint64_t ist[7];        /* ist[0] is IST1 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* >= the TSS limit means "no I/O permission bitmap" */
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "TSS must be 104 bytes");

#define IST_STACK_SIZE 16384

/* Static, 16-byte aligned, in .bss: no allocator needed this early. */
static uint8_t ist_stack_df[IST_STACK_SIZE]  __attribute__((aligned(16)));
static uint8_t ist_stack_nmi[IST_STACK_SIZE] __attribute__((aligned(16)));

static struct tss tss;

/* null, kcode, kdata, then the TSS descriptor (low + high 8 bytes).
 * Must be WRITABLE memory: ltr marks the TSS descriptor "busy".
 * The code and data descriptors are written with the accessed bit already set,
 * loading their selectors never needs to write to the table. */
static uint64_t gdt[5] __attribute__((aligned(16)));

#define GDT_KCODE_DESC 0x00AF9B000000FFFFull    /* P, DPL0, code, R, accessed; L=1 (64-bit), G */
#define GDT_KDATA_DESC 0x00CF93000000FFFFull    /* P, DPL0, data, RW, accessed; G */

static void set_tss_descriptor(int slot, uint64_t base, uint32_t limit) {
    gdt[slot] = (limit & 0xFFFFull)
              | ((base & 0xFFFFFFull) << 16)
              | (0x89ull << 40)                      /* present, DPL0, 64-bit TSS (available) */
              | ((uint64_t)((limit >> 16) & 0xF) << 48)
              | (((base >> 24) & 0xFFull) << 56);
    gdt[slot + 1] = base >> 32;
}

/* lgdt does not touch CS. A far return is how CS actually changes. */
static void reload_segments(void) {
    __asm__ volatile (
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [cs] "i"(GDT_KCODE), [ds] "i"(GDT_KDATA)
        : "rax", "memory");
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = GDT_KCODE_DESC;
    gdt[2] = GDT_KDATA_DESC;

    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(ist_stack_df  + sizeof ist_stack_df);
    tss.ist[IST_NMI - 1]          = (uint64_t)(ist_stack_nmi + sizeof ist_stack_nmi);
    tss.iomap_base = sizeof tss;

    set_tss_descriptor(3, (uint64_t)&tss, sizeof tss - 1);

    struct dt_ptr ptr = { .limit = sizeof gdt - 1, .base = (uint64_t)gdt };
    cpu_lgdt(&ptr);
    reload_segments();
    cpu_ltr(GDT_TSS);
}

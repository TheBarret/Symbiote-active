#include <stdint.h>
#include <core/arch/cpu.h>
#include <core/arch/gdt.h>

/* 64-bit Task State Segment (AMD64 architecture). */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];        /* Stack pointers for privilege level changes (RSP0, RSP1, RSP2) */
    uint64_t reserved1;
    uint64_t ist[7];        /* Interrupt Stack Table (ist[0] = IST1, etc.) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* I/O map base address (set to sizeof(tss) to disable I/O bitmap) */
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "TSS must be 104 bytes");

#define IST_STACK_SIZE 16384 /* 16 KiB dedicated stack per IST slot */

/* Static, 16-byte aligned stacks in .bss */
static uint8_t ist_stack_df[IST_STACK_SIZE]  __attribute__((aligned(16)));
static uint8_t ist_stack_nmi[IST_STACK_SIZE] __attribute__((aligned(16)));

static struct tss tss;

/* GDT layout:
 * [0] Null descriptor
 * [1] Kernel Code (64-bit)
 * [2] Kernel Data
 * [3] TSS descriptor (low 64 bits)
 * [4] TSS descriptor (high 64 bits)
 */
static uint64_t gdt[5] __attribute__((aligned(16)));

/* Descriptor bit definitions */
#define GDT_KCODE_DESC 0x00AF9B000000FFFFull /* P=1, DPL=0, S=1, Type=0xB (Code/Read/Accessed), L=1 (64-bit), G=1 */
#define GDT_KDATA_DESC 0x00CF93000000FFFFull /* P=1, DPL=0, S=1, Type=0x3 (Data/Read/Write/Accessed), G=1 */

static void set_tss_descriptor(int slot, uint64_t base, uint32_t limit) {
    gdt[slot] = (limit & 0xFFFFull)
              | ((base & 0xFFFFFFull) << 16)
              | (0x89ull << 40)                    /* Present (0x80), DPL 0, 64-bit TSS available (0x9) */
              | ((uint64_t)((limit >> 16) & 0xF) << 48)
              | (((base >> 24) & 0xFFull) << 56);
    gdt[slot + 1] = base >> 32;                     /* Upper 32 bits of base address */
}

/* Reload code segment via far return and data segments normally. */
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

    /* Stack grows downwards; assign top of stack to IST entries */
    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(ist_stack_df  + sizeof ist_stack_df);
    tss.ist[IST_NMI - 1]          = (uint64_t)(ist_stack_nmi + sizeof ist_stack_nmi);
    tss.iomap_base = sizeof tss;

    set_tss_descriptor(3, (uint64_t)&tss, sizeof tss - 1);

    struct dt_ptr ptr = { .limit = sizeof(gdt) - 1, .base = (uint64_t)gdt };
    cpu_lgdt(&ptr);
    reload_segments();
    cpu_ltr(GDT_TSS);
}

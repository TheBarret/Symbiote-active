#ifndef CORE_CPU_H
#define CORE_CPU_H

#include <stdint.h>

/*  Time  */

/* TSC field. Non-serializing; sufficient for an estimated timestamp. */
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/* Discover TSC frequency in Hz.
 * Should be at CPUID 0x15, a family/model table for the ECX==0 case.
 * Returns 0 if no estimate is available. No panics, no blocks. */
uint64_t cpu_tsc_hz(void);

/*
 * Port I/O
 *
 * Patch: every port access now carries a "memory" clobber. A port write
 * can have observable side effects on memory (device DMA, MMIO-adjacent
 * registers), and a sequence of port accesses must not be reordered by
 * the compiler relative to memory. The clobber is a compiler barrier,
 * not a CPU fence, so it costs nothing at runtime.
 *  */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Patch: added for symmetry with inw. */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Note: Moved up from the "PCI handlers" section.
 * outl/inl are general 32-bit port I/O; PCI happens to use them,
 * but so does anything else that talks to a 32-bit port. */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Read `count` 16-bit words from a port into memory (ATA PIO uses this). */
static inline void insw(uint16_t port, void *addr, uint64_t count) {
    __asm__ volatile ("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *addr, uint64_t count) {
    __asm__ volatile ("rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

/*
 * Interrupt control
 *  */

/* cli/sti also act as compiler barriers ("memory"):
 * code that checks a flag an interrupt handler sets must not be reordered
 * across them. */
static inline void cpu_cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile ("sti" ::: "memory"); }

/* Patch: hlt now carries a "memory" clobber. It is a synchronisation
 * point; code after the halt may depend on data the wakeup interrupt
 * wrote, and the compiler must not move that data across the hlt. */
static inline void cpu_hlt(void) { __asm__ volatile ("hlt" ::: "memory"); }

/* sti immediately followed by hlt, in ONE asm statement.
 * The CPU delays interrupt delivery until after the instruction following sti,
 * so a wakeup cannot slip in between the two and be lost.
 * This is the only race-free way to sleep until an interrupt. Use it as:
 *     cpu_cli(); while (!condition) { cpu_sti_hlt(); cpu_cli(); } ...
 */
static inline void cpu_sti_hlt(void) { __asm__ volatile ("sti; hlt" ::: "memory"); }

#define CPU_RFLAGS_IF (1ull << 9)

/* Disable interrupts and return the previous RFLAGS;
 * give that value back to cpu_irq_restore() to re-enable ONLY if they were enabled before.
 * This is what a function should use instead of a bare cpu_sti():
 * it must not turn interrupts on behind a caller that had them off. */
static inline uint64_t cpu_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void cpu_irq_restore(uint64_t flags) {
    if (flags & CPU_RFLAGS_IF)
        cpu_sti();
}

/*
 * Control registers and paging
 *
 * Patch: read_cr2/read_cr3/write_cr3/invlpg now carry a "memory" clobber.
 * A CR3 write changes the entire address-space mapping,
 * and an invlpg invalidates a TLB entry. Without the clobber,
 * the compiler is free to hoist a store above the CR3 write,
 * which is exactly the kind of bug that shows up once and never reproduces.
 *  */

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v) : : "memory");
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

static inline void invlpg(const void *addr) {
    __asm__ volatile ("invlpg (%0)" :: "r"(addr) : "memory");
}

/*
 * Descriptor tables
 *  */

/* Descriptor-table register image used by lgdt / lidt. */
struct dt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static inline void cpu_lgdt(const struct dt_ptr *p) {
    __asm__ volatile ("lgdt %0" :: "m"(*p) : "memory");
}

static inline void cpu_lidt(const struct dt_ptr *p) {
    __asm__ volatile ("lidt %0" :: "m"(*p) : "memory");
}

/* Patch: kept the "memory" clobber that was already here.
 * Loading the task register changes what the CPU will do on the next privilege transition; nothing may be reordered across it. */
static inline void cpu_ltr(uint16_t selector) {
    __asm__ volatile ("ltr %0" :: "r"(selector) : "memory");
}

/*
 * Halt
 *  */

/* Stop this CPU for good. Interrupts off first so nothing can wake it. */
__attribute__((noreturn))
static inline void cpu_halt_forever(void) {
    cpu_cli();
    for (;;)
        __asm__ volatile ("hlt");
}

/*
 * CPUID
 *  */

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                              : "a"(leaf), "c"(subleaf));
}

#endif

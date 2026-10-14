#include <stddef.h>
#include <stdint.h>
#include <core/boot.h>
#include <core/cpu.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/pmm.h>
#include <core/vmm.h>
#include <lib/mem.h>

/* x86_64 4-level paging. Limine boots us in 4-level mode by default. */

#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITE     (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_PWT       (1ull << 3)
#define PTE_PCD       (1ull << 4)
#define PTE_ACCESSED  (1ull << 5)
#define PTE_DIRTY     (1ull << 6)
#define PTE_HUGE      (1ull << 7)   /* page-size bit on PD/PDPT */
#define PTE_GLOBAL    (1ull << 8)
#define PTE_NX        (1ull << 63)

#define PTE_ADDR_MASK 0x000ffffffffff000ull

/* Linker script symbols (virtual addresses). */
extern char __kernel_start[];
extern char __limine_reqs_start[];
extern char __limine_reqs_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __kernel_end[];

static uint64_t root_phys;   /* our PML4 physical address */
static bool     nx_ok;       /* EFER.NXE is set */
static bool     ready;       /* vmm_init() has completed */

static inline uint64_t *table_virt(uint64_t phys) {
    return (uint64_t *)pmm_phys_to_virt(phys);
}

static uint64_t alloc_table(void) {
    uint64_t phys = pmm_alloc();
    if (phys == 0)
        PANIC("vmm: out of frames for page tables");
    memset(pmm_phys_to_virt(phys), 0, VMM_PAGE_SIZE);
    return phys;
}

/* Convert VMM_* flags into PTE bits. Present is always set. */
static uint64_t flags_to_pte(uint64_t flags) {
    uint64_t pte = PTE_PRESENT;
    if (flags & VMM_WRITE)
        pte |= PTE_WRITE;
    if (flags & VMM_USER)
        pte |= PTE_USER;
    if (flags & VMM_GLOBAL)
        pte |= PTE_GLOBAL;
    if ((flags & VMM_NX) && nx_ok)
        pte |= PTE_NX;
    if (flags & VMM_NOCACHE)
        pte |= PTE_PCD | PTE_PWT;
    return pte;
}

static bool efer_nxe_enabled(void) {
    uint32_t eax, edx;
    /* RDMSR IA32_EFER (0xC0000080); bit 11 = NXE */
    __asm__ volatile ("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080u));
    return (eax & (1u << 11)) != 0;
}

/* Recursively clone one page-table level from Limine into a fresh PMM frame.
 * level: 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT. */
static uint64_t clone_level(uint64_t old_phys, int level) {
    uint64_t new_phys = alloc_table();
    uint64_t *dst = table_virt(new_phys);
    uint64_t *src = table_virt(old_phys);

    for (int i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & PTE_PRESENT))
            continue;

        /* Leaf: 4 KiB PTE, or a huge page at PD/PDPT. Copy as-is. */
        if (level == 1 || (e & PTE_HUGE)) {
            dst[i] = e;
            continue;
        }

        uint64_t child = clone_level(e & PTE_ADDR_MASK, level - 1);
        dst[i] = (e & ~PTE_ADDR_MASK) | child;
    }
    return new_phys;
}

/* Walk to the 4 KiB PTE for virt. Returns NULL if a huge page blocks the path or (when create==false) a table is missing.
 * When create==true, allocates missing intermediate tables. */
static uint64_t *walk_pte(uint64_t virt, bool create) {
    uint64_t indices[4] = {
        (virt >> 39) & 0x1ff,
        (virt >> 30) & 0x1ff,
        (virt >> 21) & 0x1ff,
        (virt >> 12) & 0x1ff,
    };

    uint64_t table_phys = root_phys;

    for (int level = 0; level < 3; level++) {
        uint64_t *table = table_virt(table_phys);
        uint64_t *entry = &table[indices[level]];

        if (!(*entry & PTE_PRESENT)) {
            if (!create)
                return NULL;
            uint64_t child = alloc_table();
            /* Intermediate tables: present + writable so later leaves can vary.
             * Supervisor-only (no USER) keeps user mode from walking them. */
            *entry = child | PTE_PRESENT | PTE_WRITE;
        } else if (*entry & PTE_HUGE) {
            return NULL;    /* cannot punch a 4 KiB hole through a huge page */
        }

        table_phys = *entry & PTE_ADDR_MASK;
    }

    uint64_t *pt = table_virt(table_phys);
    return &pt[indices[3]];
}

static void apply_range(uint64_t start, uint64_t end, uint64_t flags) {
    start &= ~(uint64_t)(VMM_PAGE_SIZE - 1);
    end = (end + VMM_PAGE_SIZE - 1) & ~(uint64_t)(VMM_PAGE_SIZE - 1);

    for (uint64_t va = start; va < end; va += VMM_PAGE_SIZE) {
        uint64_t *pte = walk_pte(va, false);
        if (pte == NULL || !(*pte & PTE_PRESENT))
            continue;   /* not mapped at 4 KiB; leave Limine huge pages alone */
        if (*pte & PTE_HUGE)
            continue;

        uint64_t phys = *pte & PTE_ADDR_MASK;
        *pte = phys | flags_to_pte(flags);
        invlpg((void *)va);
    }
}

/* Re-apply section permissions so W^X holds even if Limine was looser. */
static void apply_kernel_wx(void) {
    /* Limine requests: RW, not executable. */
    apply_range((uint64_t)__limine_reqs_start, (uint64_t)__limine_reqs_end,
                VMM_WRITE | VMM_NX);
    /* .text: RX */
    apply_range((uint64_t)__text_start, (uint64_t)__text_end, 0);
    /* .rodata + extensions: R, NX */
    apply_range((uint64_t)__rodata_start, (uint64_t)__rodata_end, VMM_NX);
    /* .data + .bss: RW, NX */
    apply_range((uint64_t)__data_start, (uint64_t)__data_end,
                VMM_WRITE | VMM_NX);
}

void vmm_init(void) {
    if (!boot_memory_ok())
        PANIC("memmap/hhdm missing (pmm_init first)");

    nx_ok = efer_nxe_enabled();
    if (!nx_ok)
        klog_warning("VMM: EFER.NXE clear; NX flag will be ignored\n");

    uint64_t old_cr3 = read_cr3() & PTE_ADDR_MASK;
    root_phys = clone_level(old_cr3, 4);

    /* Switch to our tables. Reloading CR3 flushes the TLB. */
    write_cr3(root_phys);

    apply_kernel_wx();

    klog("→ vmm_init() root=%p nx=%d kernel=%p..%p\n",
            (void *)root_phys, nx_ok ? 1 : 0,
            (void *)__kernel_start, (void *)__kernel_end);

    ready = true;
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    if ((virt | phys) & (VMM_PAGE_SIZE - 1))
        return false;

    uint64_t *pte = walk_pte(virt, true);
    if (pte == NULL)
        return false;
    if (*pte & PTE_PRESENT)
        return false;

    *pte = (phys & PTE_ADDR_MASK) | flags_to_pte(flags);
    invlpg((void *)virt);
    return true;
}

bool vmm_unmap(uint64_t virt) {
    if (virt & (VMM_PAGE_SIZE - 1))
        return false;

    uint64_t *pte = walk_pte(virt, false);
    if (pte == NULL || !(*pte & PTE_PRESENT))
        return false;

    *pte = 0;
    invlpg((void *)virt);
    return true;
}

bool vmm_protect(uint64_t virt, uint64_t flags) {
    if (virt & (VMM_PAGE_SIZE - 1))
        return false;

    uint64_t *pte = walk_pte(virt, false);
    if (pte == NULL || !(*pte & PTE_PRESENT))
        return false;

    uint64_t phys = *pte & PTE_ADDR_MASK;
    *pte = phys | flags_to_pte(flags);
    invlpg((void *)virt);
    return true;
}

uint64_t vmm_translate(uint64_t virt) {
    uint64_t indices[4] = {
        (virt >> 39) & 0x1ff,
        (virt >> 30) & 0x1ff,
        (virt >> 21) & 0x1ff,
        (virt >> 12) & 0x1ff,
    };

    uint64_t table_phys = root_phys;
    for (int level = 0; level < 4; level++) {
        uint64_t *table = table_virt(table_phys);
        uint64_t e = table[indices[level]];
        if (!(e & PTE_PRESENT))
            return 0;

        /* Huge page at PDPT (1 GiB, level 1) or PD (2 MiB, level 2). */
        if (level == 1 && (e & PTE_HUGE))
            return (e & 0x000fffffc0000000ull) + (virt & 0x3fffffffull);
        if (level == 2 && (e & PTE_HUGE))
            return (e & 0x000fffffffe00000ull) + (virt & 0x1fffffull);
        if (level == 3)
            return (e & PTE_ADDR_MASK) + (virt & 0xfffull);

        table_phys = e & PTE_ADDR_MASK;
    }
    return 0;
}

uint64_t vmm_root_phys(void) {
    return root_phys;
}

bool vmm_ready(void) {
    return ready;
}

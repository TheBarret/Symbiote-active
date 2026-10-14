#ifndef CORE_VMM_H
#define CORE_VMM_H

#include <stdbool.h>
#include <stdint.h>
#include <core/version.h>

/* Virtual memory manager.
 *
 * Takes ownership of the address space by cloning Limine's page tables into PMM-backed frames,
 * then switching CR3. After init, bootloader page tables are unused and may be reclaimed with the rest of BOOTLOADER_RECLAIMABLE.
 * Map/unmap operate on 4 KiB pages.
 * Huge pages inherited from Limine (HHDM) are left intact; do not try to map/unmap inside those ranges.
 * No locking, same single-threaded boot assumption as the PMM. */

// Moved to ref: core/version.h
//#define VMM_PAGE_SIZE 4096

/* PTE flag bits passed to vmm_map / vmm_protect. Present is always implied. */
#define VMM_WRITE   (1ull << 0)
#define VMM_USER    (1ull << 1)
#define VMM_NX      (1ull << 2)
#define VMM_GLOBAL  (1ull << 3)

/* Higher-half window reserved for kernel dynamic mappings (heap, etc.).
 * Kept clear of HHDM and the kernel image at 0xffffffff80000000. */
#define VMM_DYNAMIC_BASE 0xfffffe0000000000ull

// device memory must not be cached
#define VMM_NOCACHE (1ull << 4)

/* Clone Limine's tables, switch CR3, apply W^X on kernel sections.
 * Requires pmm_init() first. Panics on failure. */
void vmm_init(void);

/* Map one 4 KiB page: virt -> phys with the given flags.
 * Returns false if the VA is already mapped, misaligned, or tables cannot be allocated. */
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Remove the mapping at virt. Does not free the physical frame.
 * Returns false if there was no 4 KiB mapping (missing or huge). */
bool vmm_unmap(uint64_t virt);

/* Change flags on an existing 4 KiB mapping. Physical address is unchanged. */
bool vmm_protect(uint64_t virt, uint64_t flags);

/* Walk the active tables. Returns the physical address, or 0 if unmapped.
 * Works for 4 KiB and huge pages (returns the page-aligned phys covering virt). */
uint64_t vmm_translate(uint64_t virt);

/* Physical address of the active PML4 (CR3 without PCID/flags). */
uint64_t vmm_root_phys(void);

/* True once vmm_init() has finished. The fault reporter asks this before it calls vmm_translate(),
 * which is only meaningful on our own tables. */
bool vmm_ready(void);

#endif

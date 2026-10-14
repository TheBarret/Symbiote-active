#ifndef CORE_PMM_H
#define CORE_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/version.h"

/* Physical memory manager.
 *
 * A bitmap with one bit per 4 KiB frame.
 * The bitmap itself lives in a usable region of physical memory, reached via the HHDM.
 * Init walks the Limine memory map:
 *   - marks every frame used,
 *   - marks USABLE regions free,
 *   - reserves the bitmap, the kernel image, and the framebuffer,
 *   - reserves frame 0 so pmm_alloc() returning 0 is unambiguously "out".
 *
 * No locking, single-threaded boot assumption, same as the rest of the kernel.
 * When SMP arrives this file is one of the places that will need a spinlock. */

// Moved to ref: core/version.h
//#define PMM_PAGE_SIZE 4096

/* Initialize from the bootloader's memory map and HHDM.
 * Panics if the map or HHDM is missing, or if there is no usable region big enough to hold the bitmap. */
void pmm_init(void);

/* Allocate one 4 KiB frame. Returns its physical address, or 0 if no frame is available.
 * The frame's contents are undefined; callers that want zeroed memory must clear it themselves. */
uint64_t pmm_alloc(void);

/* Allocate `count` consecutive frames. Returns the physical address of the first, or 0 on failure.
 * Used by the heap for chunk allocation. */
uint64_t pmm_alloc_contig(size_t count);

/* Return `count` consecutive frames starting at `phys` to the pool.
 * phys must have been returned by pmm_alloc()/pmm_alloc_contig() and count must match the original request.
 * Out-of-range or misaligned addresses are ignored (see ref: pmm.c). */
void pmm_free(uint64_t phys, size_t count);

/* HHDM helpers. Available after pmm_init; they just forward to boot.c. */
void *pmm_phys_to_virt(uint64_t phys);
uint64_t pmm_virt_to_phys(const void *virt);

/* Statistics. Both fields are counted in frames. */
struct pmm_stats {
    size_t total_frames;    /* frames the bitmap covers */
    size_t free_frames;     /* frames currently free */
};
struct pmm_stats pmm_get_stats(void);

#endif

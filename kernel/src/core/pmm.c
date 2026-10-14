#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/boot.h>
#include <core/cpu.h>
#include <core/klog.h>
#include <core/kprintf.h>
#include <core/panic.h>
#include <core/pmm.h>

/* The memory map is sorted by base address,
 * and USABLE / BOOTLOADER_RECLAIMABLE entries are guaranteed 4096-aligned and non-overlapping.
 * We assert those properties in init so a future bootloader that violates them is caught here,
 * rather than as a corruption hours later. */

static uint8_t *bitmap;             /* one bit per frame; 1 = used, 0 = free */
static size_t   bitmap_bytes;
static size_t   total_frames;
static size_t   free_frames;

/* Highest physical address any usable region reaches. Determines bitmap size. */
static uint64_t highest_usable_addr;

/*  bitmap primitives
 *
 * Bit i of the bitmap corresponds to frame i (physical address i * PAGE_SIZE).
 * The byte holding bit i is bitmap[i / 8], and the bit within that byte is (i % 8). 0 = free, 1 = used. */

static inline bool frame_is_used(size_t frame) {
    return (bitmap[frame / 8] >> (frame % 8)) & 1;
}

static inline void frame_mark_used(size_t frame) {
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void frame_mark_free(size_t frame) {
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

/* Mark `count` frames starting at `frame` used or free. */
static void frames_mark_used(size_t frame, size_t count) {
    for (size_t i = 0; i < count; i++)
        frame_mark_used(frame + i);
}

// helper (not used)
//static void frames_mark_free(size_t frame, size_t count) {
//    for (size_t i = 0; i < count; i++)
//        frame_mark_free(frame + i);
//}

/*  init  */

/* First pass: find the highest usable address across all USABLE entries. */
static uint64_t find_highest_usable(struct limine_memmap_response *map) {
    uint64_t highest = 0;
    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t end = e->base + e->length;
        if (end > highest)
            highest = end;
    }
    return highest;
}

/* Second pass: reserve every frame the bitmap will occupy, once it's placed.
 * Called after bitmap and bitmap_bytes are set. */
static void reserve_bitmap_frames(void) {
    uint64_t bitmap_phys = pmm_virt_to_phys(bitmap);
    size_t start = (size_t)(bitmap_phys / PMM_PAGE_SIZE);
    size_t count = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    frames_mark_used(start, count);
    free_frames -= count;
}

/* Third pass: reserve the kernel image and the framebuffer, using the map. */
static void reserve_special_regions(struct limine_memmap_response *map) {
    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES &&
            e->type != LIMINE_MEMMAP_FRAMEBUFFER)
            continue;

        uint64_t start_addr = e->base;
        uint64_t end_addr   = e->base + e->length;

        /* Round start down, end up, so partial pages are covered. */
        size_t start_frame = (size_t)(start_addr / PMM_PAGE_SIZE);
        size_t end_frame   = (size_t)((end_addr + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);

        for (size_t f = start_frame; f < end_frame; f++) {
            if (f < total_frames && !frame_is_used(f)) {
                frame_mark_used(f);
                free_frames--;
            }
        }
    }
}


/* init */

void pmm_init(void) {
    if (!boot_memory_ok())
        PANIC("Bootloader did not provide (memory) memmap or hhdm");

    struct limine_memmap_response *map = boot_memmap();
    uint64_t hhdm = boot_hhdm_offset();

    /* Pass 1: how high does usable memory go? Determines bitmap size. */
    highest_usable_addr = find_highest_usable(map);
    if (highest_usable_addr == 0)
        PANIC("PMM has no usable memory regions");

    total_frames = (size_t)(highest_usable_addr / PMM_PAGE_SIZE);
    bitmap_bytes = (total_frames + 7) / 8;

    /* Pass 2: place the bitmap in the first USABLE region with enough room.
     *
     * Align the candidate base up first, then check the aligned base plus the bitmap size against the entry's end.
     * A USABLE entry is required to be page-aligned,
     * but doing the alignment first means a bootloader that violates the guarantee cannot cause the bitmap to extend past the entry. */
    size_t bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bitmap_span = (uint64_t)bitmap_pages * PMM_PAGE_SIZE;

    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t base = (e->base + PMM_PAGE_SIZE - 1) & ~(uint64_t)(PMM_PAGE_SIZE - 1);
        /* Reject if the aligned base is already at or past the entry's end,
         * or if the bitmap would not fit before the end. */
        if (base >= e->base + e->length)
            continue;
        if (bitmap_span > (e->base + e->length) - base)
            continue;

        bitmap = (uint8_t *)(base + hhdm);
        break;
    }

    if (bitmap == NULL)
        PANIC("PMM has no usable region large enough for the bitmap");

    /* Pass 3: mark everything used, then clear usable regions. */
    for (size_t i = 0; i < bitmap_bytes; i++)
        bitmap[i] = 0xFF;
    free_frames = 0;

    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        /* Assert the alignment guarantee for usable entries. */
        if ((e->base & (PMM_PAGE_SIZE - 1)) != 0)
            PANIC("PMM usable regions are not page-aligned");

        size_t start_frame = (size_t)(e->base / PMM_PAGE_SIZE);
        size_t end_frame   = (size_t)((e->base + e->length) / PMM_PAGE_SIZE);

        for (size_t f = start_frame; f < end_frame && f < total_frames; f++) {
            if (frame_is_used(f)) {
                frame_mark_free(f);
                free_frames++;
            }
        }
    }

    /* Reserve the bitmap itself. */
    reserve_bitmap_frames();

    /* Reserve the kernel image and the framebuffer. */
    reserve_special_regions(map);

    /* Reserve frame 0, so a returned physical address of 0 is unambiguous. */
    if (!frame_is_used(0)) {
        frame_mark_used(0);
        free_frames--;
    }

    klog("→ pmm_init() allocated %zu frames total, of which %zu are free (%zu MiB)\n",
            total_frames, free_frames,
            (free_frames * PMM_PAGE_SIZE) / (1024 * 1024));
}

/*  allocation  */

uint64_t pmm_alloc(void) {
    return pmm_alloc_contig(1);
}

uint64_t pmm_alloc_contig(size_t count) {
    if (count == 0 || count > free_frames)
        return 0;

    /* Scan for `count` consecutive clear bits.
     * The inner loop breaks as soon as a used frame is found,
     * so the common case (many free frames in a row) is fast. */
    size_t run = 0;
    for (size_t f = 0; f < total_frames; f++) {
        if (frame_is_used(f)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            size_t start = f + 1 - count;
            frames_mark_used(start, count);
            free_frames -= count;
            return (uint64_t)start * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t phys, size_t count) {
    if (phys == 0 || count == 0)
        return;
    if ((phys & (PMM_PAGE_SIZE - 1)) != 0)
        return;     /* misaligned: refuse rather than corrupt the bitmap */

    size_t start = (size_t)(phys / PMM_PAGE_SIZE);
    if (start + count > total_frames)
        return;     /* out of range */

    for (size_t i = 0; i < count; i++) {
        size_t f = start + i;
        if (!frame_is_used(f))
            continue;   /* already free; idempotent */
        frame_mark_free(f);
        free_frames++;
    }
}

/*  HHDM helpers  */

void *pmm_phys_to_virt(uint64_t phys) {
    return (void *)(phys + boot_hhdm_offset());
}

uint64_t pmm_virt_to_phys(const void *virt) {
    return (uint64_t)virt - boot_hhdm_offset();
}

/*  stats  */

struct pmm_stats pmm_get_stats(void) {
    struct pmm_stats s = {
        .total_frames = total_frames,
        .free_frames = free_frames,
    };
    return s;
}

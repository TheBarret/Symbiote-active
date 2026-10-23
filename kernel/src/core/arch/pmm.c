#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/arch/boot.h>
#include <core/arch/cpu.h>
#include <core/arch/pmm.h>
#include <core/klog.h>
#include <core/kprintf.h>
#include <core/panic.h>


/* Limine sorts the map by base and guarantees USABLE and
 * BOOTLOADER_RECLAIMABLE entries are page-aligned and non-overlapping.
 * BOOTLOADER_RECLAIMABLE is currently retained (not reclaimed). */

static uint8_t *bitmap;             /* 1 = used, 0 = free */
static size_t   bitmap_bytes;
static size_t   total_frames;
static size_t   free_frames;
static size_t   alloc_cursor;       /* scan hint, wraps */

/*  bitmap primitives  */

static inline bool frame_is_used(size_t frame) {
    return (bitmap[frame / 8] >> (frame % 8)) & 1;
}

static inline void frame_mark_used(size_t frame) {
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void frame_mark_free(size_t frame) {
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

/* Bounds-checked. Refuses silently rather than corrupting the bitmap. */
static void frames_mark_used(size_t frame, size_t count) {
    if (frame >= total_frames || count > total_frames - frame)
        return;
    for (size_t i = 0; i < count; i++)
        frame_mark_used(frame + i);
}

/*  init  */

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

/* Caller guarantees the bitmap frames are free. Today that holds because
 * placement runs after Pass 3 has freed every USABLE frame. */
static void reserve_bitmap_frames(void) {
    uint64_t bitmap_phys = pmm_virt_to_phys(bitmap);
    size_t start = (size_t)(bitmap_phys / PMM_PAGE_SIZE);
    size_t count = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    frames_mark_used(start, count);
    /* frames_mark_used is bounds-checked and may no-op; only debit when it can mark. */
    if (start + count <= total_frames)
        free_frames -= count;
}

static void reserve_special_regions(struct limine_memmap_response *map) {
    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES &&
            e->type != LIMINE_MEMMAP_FRAMEBUFFER)
            continue;

        uint64_t start_addr = e->base;
        uint64_t end_addr   = e->base + e->length;

        size_t start_frame = (size_t)(start_addr / PMM_PAGE_SIZE);
        size_t end_frame   = (size_t)((end_addr + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);

        for (size_t f = start_frame; f < end_frame && f < total_frames; f++) {
            if (!frame_is_used(f)) {
                frame_mark_used(f);
                free_frames--;
            }
        }
    }
}

void pmm_init(void) {
    if (!boot_memory_ok())
        PANIC("Bootloader did not provide (memory) memmap or hhdm");

    struct limine_memmap_response *map = boot_memmap();
    uint64_t hhdm = boot_hhdm_offset();

    /* Pass 1: size the bitmap from the highest usable byte. Round up so the
     * frame holding the tail of a non-aligned region is covered. */
    uint64_t highest_usable_addr = find_highest_usable(map);
    if (highest_usable_addr == 0)
        PANIC("PMM has no usable memory regions");

    total_frames = (size_t)((highest_usable_addr + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    bitmap_bytes = (total_frames + 7) / 8;

    /* Pass 2: place the bitmap in the first USABLE region that fits.
     * Align the candidate base up first, then check against the entry's end,
     * so a bootloader that violates the alignment guarantee can't push the
     * bitmap past the entry. */
    size_t bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bitmap_span = (uint64_t)bitmap_pages * PMM_PAGE_SIZE;

    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t base = (e->base + PMM_PAGE_SIZE - 1) & ~(uint64_t)(PMM_PAGE_SIZE - 1);
        if (base >= e->base + e->length)
            continue;
        if (bitmap_span > (e->base + e->length) - base)
            continue;

        bitmap = (uint8_t *)(base + hhdm);
        break;
    }

    if (bitmap == NULL)
        PANIC("PMM has no usable region large enough for the bitmap");

    /* Pass 3: all used, then clear USABLE regions. */
    for (size_t i = 0; i < bitmap_bytes; i++)
        bitmap[i] = 0xFF;
    free_frames = 0;

    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        if ((e->base & (PMM_PAGE_SIZE - 1)) != 0)
            PANIC("PMM usable regions are not page-aligned");

        size_t start_frame = (size_t)(e->base / PMM_PAGE_SIZE);
        size_t end_frame   = (size_t)((e->base + e->length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);

        for (size_t f = start_frame; f < end_frame && f < total_frames; f++) {
            if (frame_is_used(f)) {
                frame_mark_free(f);
                free_frames++;
            }
        }
    }

    reserve_bitmap_frames();
    reserve_special_regions(map);

    /* Frame 0 stays reserved so pmm_alloc() returning 0 is unambiguously "out".
     * vmm_translate's 0 sentinel (unmapped vs phys 0) also depends on this. */
    if (!frame_is_used(0)) {
        frame_mark_used(0);
        free_frames--;
    }

    alloc_cursor = 0;

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

    /* Two-segment scan from alloc_cursor: [cursor, total) then [0, cursor).
     * Run state carries across the wrap. */
    size_t run = 0;
    size_t start = 0;

    for (size_t scanned = 0; scanned < total_frames; scanned++) {
        size_t f = alloc_cursor + scanned;
        if (f >= total_frames)
            f -= total_frames;

        if (frame_is_used(f)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            start = f + 1 - count;
            if (f + 1 < count)         /* wrapped during the run */
                start = total_frames - (count - (f + 1));
            frames_mark_used(start, count);
            free_frames -= count;
            alloc_cursor = (start + count) % total_frames;
            return (uint64_t)start * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t phys, size_t count) {
    if (phys == 0 || count == 0)
        return;
    if ((phys & (PMM_PAGE_SIZE - 1)) != 0)
        return;
    size_t start = (size_t)(phys / PMM_PAGE_SIZE);
    if (start >= total_frames || count > total_frames - start)
        return;

    for (size_t i = 0; i < count; i++) {
        size_t f = start + i;
        if (!frame_is_used(f))
            continue;
        frame_mark_free(f);
        free_frames++;
    }
}

/*  HHDM helpers  */

/* Both are only valid for pointers inside the HHDM window. */
void *pmm_phys_to_virt(uint64_t phys) {
    return (void *)(phys + boot_hhdm_offset());
}

uint64_t pmm_virt_to_phys(const void *virt) {
    return (uint64_t)virt - boot_hhdm_offset();
}

struct pmm_stats pmm_get_stats(void) {
    struct pmm_stats s = {
        .total_frames = total_frames,
        .free_frames = free_frames,
    };
    return s;
}

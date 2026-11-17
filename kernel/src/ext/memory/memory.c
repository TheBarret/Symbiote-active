#include <stddef.h>
#include <lib/string.h>
#include <lib/mem.h>
#include <core/arch/boot.h>
#include <core/arch/pmm.h>
#include <core/arch/heap.h>
#include <core/arch/vmm.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/kprintf.h>

/*  mem info  */

static const char *memmap_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi-reclaim";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi-nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "boot-reclaim";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved-mapped";
    default:                                   return "unknown";
    }
}

/* Bytes -> "N MiB" or "N KiB" or "N B", whichever fits best. */
static void fmt_size(uint64_t bytes, char *out, size_t out_sz) {
    if (bytes >= 1024ull * 1024ull) {
        uint64_t mib = bytes / (1024ull * 1024ull);

        // incorrect decimals
        //uint64_t rem = (bytes % (1024ull * 1024ull)) / 1024ull;
        uint64_t rem = (bytes % (1024ull * 1024ull)) * 1000ull / (1024ull * 1024ull);

        ksnprintf(out, out_sz, "%llu.%03llu MiB",
                  (unsigned long long)mib, (unsigned long long)rem);
    } else if (bytes >= 1024) {
        ksnprintf(out, out_sz, "%llu KiB", (unsigned long long)(bytes / 1024));
    } else {
        ksnprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
    }
}

static int cmd_mem_fn(const struct cmd_args *a) {
    (void)a;

    if (!boot_memory_ok()) {
        kprintf("mem: bootloader did not provide memmap or hhdm\n");
        return 1;
    }

    struct limine_memmap_response *map = boot_memmap();
    uint64_t hhdm = boot_hhdm_offset();

    uint64_t total_usable = 0;
    size_t usable_regions = 0;
    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            total_usable += e->length;
            usable_regions++;
        }
    }

    kprintf("Memory\n");
    kprintf("HHDM offset  : 0x%016llx\n", (unsigned long long)hhdm);
    kprintf("Usable total : %llu KiB (%llu MiB)\n",
            (unsigned long long)(total_usable / 1024),
            (unsigned long long)(total_usable / (1024 * 1024)));

    struct pmm_stats p = pmm_get_stats();
    kprintf("Frames       : %zu total, %zu free (%zu MiB free)\n",
            p.total_frames, p.free_frames,
            (p.free_frames * PMM_PAGE_SIZE) / (1024 * 1024));

    struct heap_stats h = heap_get_stats();
    kprintf("Heap         : %zu chunks, %zu blocks (%zu free)\n",
            h.chunks, h.blocks, h.blocks_free);
    kprintf("Heap bytes   : %zu used, %zu free, largest %zu\n",
            h.bytes_used, h.bytes_free, h.largest_free);

    kprintf("Mapping\n");
    kprintf("%zu region(s), %zu usable\n", map->entry_count, usable_regions);
    kprintf("  idx  start               end                 type             size\n");
    kprintf("  ---  ------------------  ------------------  ---------------  -----------------\n");

    for (size_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        uint64_t start = e->base;
        uint64_t end   = e->base + e->length;

        char sz[24];
        fmt_size(e->length, sz, sizeof sz);

        /* Sanity: end must equal base + length. Flag if not. */
        const char *flag = (end - start == e->length) ? "" : "  !! len mismatch";

        kprintf("  [%2zu] 0x%016llx  0x%016llx  %-15s  %s%s\n",
                i,
                (unsigned long long)start,
                (unsigned long long)end,
                memmap_type_name(e->type),
                sz,
                flag);
    }
    return CMD_OK;
}

SYM_COMMAND(mem, "", "Display memory status", cmd_mem_fn);

#include <stddef.h>
#include <lib/string.h>
#include <lib/mem.h>
#include <core/arch/boot.h>
#include <core/arch/pmm.h>
#include <core/arch/vmm.h>
#include <core/arch/heap.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/kprintf.h>

/*  md - memory dump console tool
 *  Modes:
 *   md [scan]                         print the memory map as a menu of regions
 *   md <n> [offset] [len]             dump from region <n> (indexed, cannot fault)
 *   md addr <hex> [len]               dump from an explicit VA (validated)
 *   md phys <hex> [len]               dump from an explicit physical address (HHDM-translated, validated)
 *
 * The indexed form is the primary one:
 * coordinates come from the memory map, not from user-typed hex, so the common case cannot fault.
 * The addr/phys forms are escape hatches for debugging specific addresses;
 * both validate through vmm_translate and refuse cleanly rather than run an panic.
 *
 */

#define MD_DEFAULT_LEN    256u
#define MD_MAX_LEN        4096u
#define MD_BYTES_PER_ROW 16u
#define MD_MAX_REGIONS    64u       /* sanity bound on the memmap walk */

/* Is this region type HHDM-covered (i.e. readable via md <n>)? */
static bool region_is_readable(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    case LIMINE_MEMMAP_FRAMEBUFFER:
    case LIMINE_MEMMAP_RESERVED_MAPPED:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:
        return true;
    default:
        return false;
    }
}

/* Short type name for the scan table and for `md <n>` headers. */
static const char *region_type_name(uint64_t type) {
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

/*  Hex/ascii dump. Caller guarantees [base, base+len) is readable. */
static void md_dump(uint64_t va_base, uint64_t len) {
    const uint8_t *ptr = (const uint8_t *)(uintptr_t)va_base;

    for (uint64_t i = 0; i < len; i += MD_BYTES_PER_ROW) {
        kprintf("%016llx  ", (unsigned long long)(va_base + i));

        /* Hex column, with a spacer between the two groups of 8. */
        for (uint64_t j = 0; j < MD_BYTES_PER_ROW; j++) {
            if (i + j < len)
                kprintf("%02x ", ptr[i + j]);
            else
                kprintf("   ");
            if (j == 7)
                kprintf(" ");
        }

        kprintf(" |");

        /* ASCII column. */
        for (uint64_t j = 0; j < MD_BYTES_PER_ROW && (i + j) < len; j++) {
            uint8_t c = ptr[i + j];
            kprintf("%c", (c >= 32 && c < 127) ? c : '.');
        }

        kprintf("|\n");
    }
}

/*  `md scan`: the memory map as a menu.  */
static int md_scan(void) {
    struct limine_memmap_response *map = boot_memmap();
    if (map == NULL) {
        kprintf("md: no memory map (bootloader did not answer)\n");
        return CMD_OK;
    }

    kprintf("Memory regions (index -> md <n> to dump):\n");
    kprintf("  idx   %-18s %-18s %-16s %10s   %s\n",
            "start", "end", "type", "size", "readable");
    kprintf("  ---   ------------------ ------------------ ---------------- ---------- --------\n");

    size_t shown = 0;
    for (size_t i = 0; i < map->entry_count && shown < MD_MAX_REGIONS; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        uint64_t end = e->base + e->length;
        uint64_t kib = e->length / 1024;
        bool readable = region_is_readable(e->type);

        /* Print size as KiB below 1 MiB, MiB above, matching `mem`. */
        if (e->length >= 1024 * 1024)
            kprintf("  [%2zu] 0x%016llx 0x%016llx %-16s %7llu MiB  %s\n",
                    i,
                    (unsigned long long)e->base,
                    (unsigned long long)end,
                    region_type_name(e->type),
                    (unsigned long long)(e->length / (1024 * 1024)),
                    readable ? "yes" : "no");
        else
            kprintf("  [%2zu] 0x%016llx 0x%016llx %-16s %7llu KiB  %s\n",
                    i,
                    (unsigned long long)e->base,
                    (unsigned long long)end,
                    region_type_name(e->type),
                    (unsigned long long)kib,
                    readable ? "yes" : "no");
        shown++;
    }

    if (map->entry_count > shown)
        kprintf("  ... (%zu more regions, truncated)\n", map->entry_count - shown);

    kprintf("\nUsage: continue with 'md <index>' dumps base of region marked yes\n");
    return CMD_OK;
}

/*  `md <n> [offset] [len]`: indexed dump, cannot fault.  */
static int md_region(uint64_t idx, uint64_t offset, uint64_t len) {
    struct limine_memmap_response *map = boot_memmap();
    if (map == NULL) {
        kprintf("md: no memory map\n");
        return CMD_USAGE;
    }
    if (idx >= map->entry_count) {
        kprintf("md: region %llu out of range (map has %zu entries; try 'md scan')\n",
                (unsigned long long)idx, map->entry_count);
        return CMD_USAGE;
    }

    const struct limine_memmap_entry *e = map->entries[idx];
    if (!region_is_readable(e->type)) {
        kprintf("md: region %llu is %s, not HHDM-mapped; not readable\n",
                (unsigned long long)idx, region_type_name(e->type));
        return CMD_USAGE;
    }

    if (offset >= e->length) {
        kprintf("md: offset 0x%llx is past region %llu's length (0x%llx)\n",
                (unsigned long long)offset, (unsigned long long)idx,
                (unsigned long long)e->length);
        return CMD_USAGE;
    }

    uint64_t avail = e->length - offset;
    if (len == 0) len = MD_DEFAULT_LEN;
    if (len > avail) len = avail;
    if (len > MD_MAX_LEN) len = MD_MAX_LEN;

    uint64_t hhdm = boot_hhdm_offset();
    if (hhdm == 0) {
        kprintf("md: HHDM offset unavailable\n");
        return CMD_USAGE;
    }
    uint64_t va = e->base + offset + hhdm;

    kprintf("Region %llu (%s)  phys 0x%llx + 0x%llx  ->  va 0x%llx  (%llu bytes)\n",
            (unsigned long long)idx, region_type_name(e->type),
            (unsigned long long)e->base, (unsigned long long)offset,
            (unsigned long long)va, (unsigned long long)len);

    md_dump(va, len);
    return CMD_OK;
}

/*  `md addr <hex> [len]` / `md phys <hex> [len]`: escape hatches.  */
static int md_explicit(uint64_t addr, bool is_physical, uint64_t len) {
    uint64_t va = addr;

    if (is_physical) {
        uint64_t hhdm = boot_hhdm_offset();
        if (hhdm == 0) {
            kprintf("md: HHDM offset unavailable; cannot translate physical address\n");
            return CMD_USAGE;
        }
        va = addr + hhdm;
        kprintf("(physical 0x%llx -> HHDM va 0x%llx)\n",
                (unsigned long long)addr, (unsigned long long)va);
    }

    if (!vmm_ready()) {
        kprintf("md: vmm not ready; refusing to probe unmapped memory\n");
        return CMD_USAGE;
    }

    if (len == 0) len = MD_DEFAULT_LEN;
    if (len > MD_MAX_LEN) len = MD_MAX_LEN;

    /* Validate every page touched from start to end (fixes unaligned cross-page faults) */
    uint64_t start_page = va & ~((uint64_t)VMM_PAGE_SIZE - 1);
    uint64_t end_page   = (va + len - 1) & ~((uint64_t)VMM_PAGE_SIZE - 1);
    uint64_t safe_len   = len;

    for (uint64_t p_va = start_page; p_va <= end_page; p_va += VMM_PAGE_SIZE) {
        if (vmm_translate(p_va) == 0) {
            if (p_va == start_page) {
                safe_len = 0;
            } else {
                safe_len = p_va - va;
            }
            kprintf("md: mapping ends at 0x%llx; clamping dump to %llu bytes\n",
                    (unsigned long long)p_va, (unsigned long long)safe_len);
            break;
        }
    }

    if (safe_len == 0) {
        kprintf("md: 0x%llx is not mapped (vmm_translate returned 0)\n",
                (unsigned long long)va);
        kprintf("    try 'md scan' for the list of readable regions\n");
        return CMD_USAGE;
    }

    md_dump(va, safe_len);
    return CMD_OK;
}

/*  Argument parsing  */
static int md_usage(void) {
    kprintf("usage:\n"
            "  md                    show the region menu (same as 'md scan')\n"
            "  md scan               show the region menu\n"
            "  md <n> [off] [len]    dump from region <n> (indexed, safe)\n"
            "  md addr <hex> [len]   dump from a virtual address (validated)\n"
            "  md phys <hex> [len]   dump from a physical address (via HHDM, validated)\n"
            "\n"
            "Defaults: len=%u, max len=%u. Try 'md scan' first.\n",
            (unsigned)MD_DEFAULT_LEN, (unsigned)MD_MAX_LEN);
    return CMD_USAGE;
}

static int cmd_memdump_fn(const struct cmd_args *a) {
    if (a->argc == 0)
        return md_scan();

    const char *sub = a->argv[0];

    /* "md scan" */
    if (strcmp(sub, "scan") == 0)
        return md_scan();

    /* "md addr <hex> [len]" and "md phys <hex> [len]" */
    if (strcmp(sub, "addr") == 0 || strcmp(sub, "phys") == 0) {
        bool physical = (sub[0] == 'p');
        if (a->argc < 2) return md_usage();
        uint64_t addr = 0;
        if (!kstrtoull(a->argv[1], &addr)) {
            kprintf("md: bad address '%s'\n", a->argv[1]);
            return md_usage();
        }
        uint64_t len = MD_DEFAULT_LEN;
        if (a->argc >= 3 && !kstrtoull(a->argv[2], &len)) {
            kprintf("md: bad length '%s'\n", a->argv[2]);
            return md_usage();
        }
        return md_explicit(addr, physical, len);
    }

    /* "md <n> [offset] [len]" -- the common case. */
    uint64_t idx = 0;
    if (!kstrtoull(sub, &idx)) {
        kprintf("md: '%s' is not a region index or subcommand\n", sub);
        if (sub[0] == '0' && (sub[1] == 'x' || sub[1] == 'X'))
            kprintf("    did you mean 'md addr %s'?\n", sub);
        return md_usage();
    }

    uint64_t offset = 0, len = MD_DEFAULT_LEN;
    if (a->argc >= 2 && !kstrtoull(a->argv[1], &offset)) {
        kprintf("md: bad offset '%s'\n", a->argv[1]);
        return md_usage();
    }
    if (a->argc >= 3 && !kstrtoull(a->argv[2], &len)) {
        kprintf("md: bad length '%s'\n", a->argv[2]);
        return md_usage();
    }
    return md_region(idx, offset, len);
}

SYM_COMMAND(md, "[a:str] [b:str] [c:str]", "Memory dump tool (args: md scan, md <n> [off] [len], md addr <hex>, md phys <hex>)", cmd_memdump_fn);

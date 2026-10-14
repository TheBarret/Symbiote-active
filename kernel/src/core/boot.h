#ifndef CORE_BOOT_H
#define CORE_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include <limine.h>

/* True if the bootloader speaks the protocol revision we asked for. */
bool boot_protocol_ok(void);

/* First framebuffer the bootloader gave us, or NULL (headless / serial-only). */
struct limine_framebuffer *boot_framebuffer(void);

/* Physical memory map. NULL if the bootloader did not answer. */
struct limine_memmap_response *boot_memmap(void);

/* Higher-Half Direct Map offset, any physical address p that is HHDM-mapped
 * can be reached at p + offset. Returns 0 if unavailable.
 * Under protocol revision 6, HHDM covers only USABLE, BOOTLOADER_RECLAIMABLE,
 * EXECUTABLE_AND_MODULES, FRAMEBUFFER, RESERVED_MAPPED, ACPI_RECLAIMABLE,  and ACPI_NVS entries.
 * RESERVED, BAD_MEMORY and arbitrary MMIO are not mapped.
 * Never dereference a physical address that is not known to be in one of those categories. */
uint64_t boot_hhdm_offset(void);

/* True if the memory map and HHDM offset are both available. */
bool boot_memory_ok(void);

/* Physical and virtual bases of the loaded kernel image, or NULL if absent.
 * Use these (not MEMMAP_EXECUTABLE_AND_MODULES) when translating kernel VAs. */
struct limine_executable_address_response *boot_executable_address(void);

#endif

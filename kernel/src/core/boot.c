#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <core/boot.h>

/* Limine & Kernel
 * The requests must not be optimised away: "used" + section placement does that,
 * and the linker script keeps the section.
 * The official limine.h provides the correct magic numbers, so we never hand-type them. */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

bool boot_protocol_ok(void) {
    return LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision);
}

struct limine_framebuffer *boot_framebuffer(void) {
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1)
        return NULL;
    return framebuffer_request.response->framebuffers[0];
}

struct limine_memmap_response *boot_memmap(void) {
    return memmap_request.response;
}

uint64_t boot_hhdm_offset(void) {
    if (hhdm_request.response == NULL)
        return 0;
    return hhdm_request.response->offset;
}

bool boot_memory_ok(void) {
    return memmap_request.response != NULL && hhdm_request.response != NULL;
}

struct limine_executable_address_response *boot_executable_address(void) {
    return executable_address_request.response;
}

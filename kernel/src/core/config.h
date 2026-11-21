#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* codebase: 0.2.0 */
/* incorrect settings can result in unpredictable behaviors!  */

/* Serial settings
 * Ref: src/core/serial.h
 */
#define COM1 0x3F8
#define SERIAL_TX_HOLD 100000   // spin-up

/* PMM settings
 * Ref: src/core/pmm.h
 */
#define PMM_PAGE_SIZE 4096

/* VMM settings
 * Ref: src/core/vmm.h
 */
#define VMM_PAGE_SIZE 4096

/* HEAP settings
 * Ref: src/core/heap.h
 */
#define HEAP_CHUNK_SIZE     (256 * 1024)
#define HEAP_ALIGN          16
#define HEAP_MIN_BLOCK_SIZE HEAP_ALIGN

/* PIC settings
 * Ref: src/core/pic.h
 */
#define PIC_VECTOR_BASE 0x20

/* Timer settings
 * Ref: src/core/timer.h
 * Ref: src/core/timer.h
 *
 * TIMER_HZ is the requested rate; the PIT divisor is an integer,
 * so the actual rate is PIT_FREQ / (PIT_FREQ / TIMER_HZ),
 * which is close to but not exactly TIMER_HZ. Query timer_hz() for the real value.
 */
#define TIMER_HZ  1000
#define PIT_FREQ  1193182ull        /* Hz, the PIT's input clock */
#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define TIMER_MAX_TASKS 16


/* Shell settings
 * Ref: src/core/shell.h
 */
#define SHELL_LINE_MAX  128

/* Console settings
 * Ref: src/core/console.h
 */
#define CONSOLE_MAX 4
#define LOG_SIZE    16384   /* 16kb, log kept in RAM for late-attaching consoles */

/* VFS settings
 * Ref: src/core/vfs.h
 */
#define VFS_PATH_MAX 128
#define VFS_NAME_MAX 32

/* Klog settings
 * Ref: src/core/klog.h
 */
#define SYM_TSC_ENABLED     false    // padding + tsc_ticks at klog_emit
#define SYM_TAG             "\x1b[1;35;7m[KERN]\x1b[0m"
#define SYM_TAG_EXT         "\x1b[1;36;7m[EXT]\x1b[0m"
#define SYM_TAG_SERV        "\x1b[1;30;7m[SERVICE]\x1b[0m"
#define SYM_TAG_INFO        "\x1b[1;34;7m[INFO]\x1b[0m"
#define SYM_TAG_WARNING     "\x1b[1;33;7m[WARNING]\x1b[0m"
#define SYM_TAG_ERROR       "\x1b[1;31;7m[ERROR]\x1b[0m"

/* fbcon settings
 * Ref: src/ext/fbcon/fbcon.h
 */
#define SYM_FBCON_BG UINT32_C(0x000a0a0a)
#define SYM_FBCON_FG UINT32_C(0x0033ff66)

/* Command dispatcher settings
 * Ref: src/core/cmd.h
 */
#define TOK_MAX 24      /* longest single parameter, e.g. "[iterations:num]" */

/* Extensions settings
 * Ref: src/core/ext.h
 * Extension load priority table
 */
enum ext_prio {
    EXT_PRIO_ALWAYS_FIRST   = 0,   // Always first in-line
    EXT_PRIO_CONSOLE        = 10,  // Output backends
    EXT_PRIO_DRIVER         = 10,  // Hardware drivers
    EXT_PRIO_FS             = 20,  // Filesystem
    EXT_PRIO_SERVICE        = 30,  // Shell applications
    EXT_PRIO_APPLET         = 40,  // Extension applications
};

/* Rnd settings
 * Ref: src/lib/random.h
 */
#define RND_RING_SIZE   256u          /* ring bytes */
#define RND_SEED        0xFF00u      /* xorshift non-zero base seed */
#define RND_SEED_ALT    0x00FFu      /* xorshift non-zero base seed fallback */

/* Router settings
 * Ref: src/core/services/router.h
 */
#define ROUTER_SIZE_MAX     16384 /* hint: use LOG_SIZE (16kb) */
#define ROUTER_MCAST_GROUPS 16u   /* Number of multicast groups; bounded by the width of router_device.mcast_groups. */
#define ROUTER_MAX_DEPTH    8u    /* Maximum nesting of router_send, stops reply cycles (A -> B -> A) */

/* Address settings
 * Ref: src/core/services/addr.h
 */
#define ADDR_CLASS_SHIFT  12
#define ADDR_VALUE_MASK   0x0FFFu

/* Bus functions */
#define NOBODY      0u      /* void(0) */
#define BUS_PRINT   0x00F1u /* print(<string>) */
#define BUS_PRINTN  0x00F3u /* print(<1/2/4-byte>) */
#define BUS_RND     0x00F4u /* uint16_t = next_rnd() */

/* Fail at build */
_Static_assert((PMM_PAGE_SIZE  & (PMM_PAGE_SIZE  - 1)) == 0, "config.h: PMM_PAGE_SIZE must be a power of two");
_Static_assert((VMM_PAGE_SIZE  & (VMM_PAGE_SIZE  - 1)) == 0, "config.h: VMM_PAGE_SIZE must be a power of two");
_Static_assert((HEAP_ALIGN     & (HEAP_ALIGN     - 1)) == 0, "config.h: HEAP_ALIGN must be a power of two");
_Static_assert(HEAP_ALIGN >= 8,                              "config.h: HEAP_ALIGN below 8 wastes header space");
_Static_assert(HEAP_MIN_BLOCK_SIZE == HEAP_ALIGN,            "config.h: HEAP_MIN_BLOCK_SIZE must equal HEAP_ALIGN");
_Static_assert(PMM_PAGE_SIZE == VMM_PAGE_SIZE,               "config.h: PMM and VMM must agree on page size");
_Static_assert(TIMER_MAX_TASKS > 0,                          "config.h: TIMER_MAX_TASKS must be positive");
#endif

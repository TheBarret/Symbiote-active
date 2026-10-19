#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* incorrect settings can result in unpredictable behaviors!  */

/* Serial settings
 * Ref: src/core/serial.c
 */
#define COM1 0x3F8

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
 * Ref: src/core/timer.c
 *
 * TIMER_HZ is the requested rate; the PIT divisor is an integer, so the
 * actual rate is PIT_FREQ / (PIT_FREQ / TIMER_HZ), which is close to but
 * not exactly TIMER_HZ. Query timer_hz() for the real value. */
#define TIMER_HZ  1000
#define PIT_FREQ  1193182ull        /* Hz, the PIT's input clock */
#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define TIMER_MAX_TASKS 16          /* bus back-end */

/* Shell settings
 * Ref: src/core/shell.c
 */
#define SHELL_LINE_MAX  128

/* Console settings
 * Ref: src/core/console.c
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
#define SYM_TAG         "\x1b[1;35;7m[KERN]\x1b[0m"
#define SYM_TAG_EXT     "\x1b[1;36;7m[EXT]\x1b[0m"
#define SYM_TAG_BUS     "\x1b[1;32;7m[BUS]\x1b[0m"
#define SYM_TAG_ENV     "\x1b[1;37;7m[ENV]\x1b[0m"
#define SYM_TAG_INFO    "\x1b[1;34;7m[INFO]\x1b[0m"
#define SYM_TAG_WARNING "\x1b[1;33;7m[WARNING]\x1b[0m"
#define SYM_TAG_ERROR   "\x1b[1;31;7m[ERROR]\x1b[0m"

/* fbcon settings
 * Theme: phosphor green on near-black (0xRRGGBB)
 * Ref: src/ext/fbcon/fbcon.c
 */
#define SYM_FBCON_BG UINT32_C(0x000a0a0a) //fallback: 0x000a0a0a;
#define SYM_FBCON_FG UINT32_C(0x0033ff66) //fallback: 0x0033ff66;

/* Bus settings
 * Ref: src/core/bus.h
 * Ref: src/core/bus.c
 */
#define BUS_MAX_DEVICES 32

/* Command dispatcher settings
 * Ref: src/core/cmd.c
 */
#define TOK_MAX 24      /* longest single parameter, e.g. "[iterations:num]" */

/* Enviroment settings
 * Ref: src/core/env.h
 */
#define ENV_IMPORTANCE_DEFAULT  5
#define ENV_IMPORTANCE_MIN      0
#define ENV_IMPORTANCE_MAX      10
#define ENV_DEFAULT_MAX_ENTRIES 512
#define ENV_DEFAULT_MAX_BYTES   (64 * 1024)
#define ENV_NAME_MAX            64
#define ENV_VALUE_MAX           1024
#define ENV_SWEEP_PERIOD_MS     1000
#define ENV_SWEEP_BATCH         64
#define ENV_MASK_DEFAULT        0x0000


/* fail at build */
_Static_assert((PMM_PAGE_SIZE  & (PMM_PAGE_SIZE  - 1)) == 0, "PMM_PAGE_SIZE must be a power of two");
_Static_assert((VMM_PAGE_SIZE  & (VMM_PAGE_SIZE  - 1)) == 0, "VMM_PAGE_SIZE must be a power of two");
_Static_assert((HEAP_ALIGN     & (HEAP_ALIGN     - 1)) == 0, "HEAP_ALIGN must be a power of two");
_Static_assert(HEAP_ALIGN >= 8,                              "HEAP_ALIGN below 8 wastes header space");
_Static_assert(HEAP_MIN_BLOCK_SIZE == HEAP_ALIGN,            "HEAP_MIN_BLOCK_SIZE must equal HEAP_ALIGN");
_Static_assert(PMM_PAGE_SIZE == VMM_PAGE_SIZE,               "PMM and VMM must agree on page size");
_Static_assert(TIMER_MAX_TASKS > 0,                          "TIMER_MAX_TASKS must be positive");
_Static_assert(BUS_MAX_DEVICES > 0,                          "BUS_MAX_DEVICES must be positive");

#endif

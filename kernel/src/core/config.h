#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* codebase: 0.1.2 */
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
#define SYM_TAG         "\x1b[1;35;7m[KERN]\x1b[0m"
#define SYM_TAG_EXT     "\x1b[1;36;7m[EXT]\x1b[0m"
#define SYM_TAG_INFO    "\x1b[1;34;7m[INFO]\x1b[0m"
#define SYM_TAG_WARNING "\x1b[1;33;7m[WARNING]\x1b[0m"
#define SYM_TAG_ERROR   "\x1b[1;31;7m[ERROR]\x1b[0m"

/* fbcon settings
 * Ref: src/ext/fbcon/fbcon.h
 */
#define SYM_FBCON_BG UINT32_C(0x000a0a0a)
#define SYM_FBCON_FG UINT32_C(0x0033ff66)

/* Command dispatcher settings
 * Ref: src/core/cmd.h
 */
#define TOK_MAX 24      /* longest single parameter, e.g. "[iterations:num]" */


/* ISA settings
 * Ref: src/core/isa.h
 * Phase = timer_ticks() % ISA_PHASE_TICKS,
 * change with care: this is the ISA's clock divider.
 */
#define ISA_PHASE_TICKS 3u /* Ticks per bus cycle. */
#define ISA_QUEUE_CAP 8u /* Fixed-capacity transaction queue, ISA reads one per cycle. */


/* fail at build */

_Static_assert((PMM_PAGE_SIZE  & (PMM_PAGE_SIZE  - 1)) == 0, "config.h: PMM_PAGE_SIZE must be a power of two");
_Static_assert((VMM_PAGE_SIZE  & (VMM_PAGE_SIZE  - 1)) == 0, "config.h: VMM_PAGE_SIZE must be a power of two");
_Static_assert((HEAP_ALIGN     & (HEAP_ALIGN     - 1)) == 0, "config.h: HEAP_ALIGN must be a power of two");
_Static_assert(HEAP_ALIGN >= 8,                              "config.h: HEAP_ALIGN below 8 wastes header space");
_Static_assert(HEAP_MIN_BLOCK_SIZE == HEAP_ALIGN,            "config.h: HEAP_MIN_BLOCK_SIZE must equal HEAP_ALIGN");
_Static_assert(PMM_PAGE_SIZE == VMM_PAGE_SIZE,               "config.h: PMM and VMM must agree on page size");
_Static_assert(TIMER_MAX_TASKS > 0,                          "config.h: TIMER_MAX_TASKS must be positive");

#endif

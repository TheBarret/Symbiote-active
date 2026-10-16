#ifndef CORE_HEAP_H
#define CORE_HEAP_H

#include <stddef.h>

/* Kernel heap: chunked free list, block headers, immediate coalescing.
 * Chunks from pmm_alloc_contig, reached via HHDM, never returned to PMM.
 * First-fit, 16-byte aligned, no locking (single-threaded boot only).
 * -DSYM_MEMDEBUG adds magic, poison, owner tagging, heapcheck.

 revisions:
 - struct block_header uses __attribute__((aligned(HEAP_ALIGN))); block_header_padded is gone.
   HDR_SIZE is now align_up(sizeof(header), 16) — 16 in fast build, 32 in debug.
 - New CHUNK_HDR_SIZE, used everywhere sizeof(struct chunk) was used in layout math. Fixes the 8-aligned first-block bug.
 - chunk_create rejects payload_bytes > SIZE_MAX - HDR_SIZE - CHUNK_HDR_SIZE.
   kmalloc guards the same expression before calling it.
 - kfree uses c->size for the bound check instead of walking to the sentinel. One walk instead of two.
 - kmalloc's scan has a seen cycle guard (debug builds only).
 - krealloc overflow check simplified; the ? SIZE_MAX trick is gone.
 - heap_get_stats uses a local ps to avoid recomputing BLOCK_PAYLOAD_SIZE(b) three times.

 * */

void heap_init(void);                        /* panics if PMM has no frames */
void *kmalloc(size_t size);                  /* 16-aligned, NULL on fail, undefined contents */
void *kzalloc(size_t size);                  /* zeroed, NULL on fail */
void kfree(void *ptr);                       /* NULL is no-op; double-free/corruption panics */
void *krealloc(void *ptr, size_t new_size);  /* NULL->kmalloc, 0->kfree+NULL, fail leaves original */

struct heap_stats {
    size_t chunks;
    size_t blocks;
    size_t blocks_free;
    size_t bytes_used;
    size_t bytes_free;
    size_t largest_free;
};
struct heap_stats heap_get_stats(void);

void heapcheck(void);                        /* no-op in fast build */

#endif

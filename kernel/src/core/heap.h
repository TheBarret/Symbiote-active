#ifndef CORE_HEAP_H
#define CORE_HEAP_H

#include <stddef.h>

/* Kernel heap.
 *
 * A chunked free list with block headers and immediate coalescing.
 * Chunks come from the PMM (pmm_alloc_contig) and are reached via the HHDM.
 * Chunks are never returned to the PMM; freeing a block returns it to the chunk's free list, where it is reused.
 *
 * Allocation is first-fit, blocks are 16-byte aligned, no locking;
 * the single-threaded boot assumption applies.
 *
 * Debug build (-DSYM_MEMDEBUG) enables:
 *   - magic numbers in block headers, verified on free and in heapcheck
 *   - poison on free (0xDD) and on alloc (0xAA)
 *   - owner tagging from ext_current() at alloc time
 *   - heapcheck() walks every chunk and every block and verifies headers
 * The fast build has none of that; the two functions below are the API in both builds. */

/* Initialize the heap with one chunk of the default size.
 * Panics if the PMM has no frames. */
void heap_init(void);

/* Allocate `size` bytes. Returns 16-byte aligned memory, or NULL.
 * Contents are undefined (use kzalloc for zeroed memory). */
void *kmalloc(size_t size);

/* Allocate `size` bytes, zeroed. Returns NULL on failure. */
void *kzalloc(size_t size);

/* Free a block. kfree(NULL) is a no-op. A double free or a corrupted
 * header panics (always, not just in debug builds) because continuing
 * with a broken heap is worse than stopping. */
void kfree(void *ptr);

/* Resize an allocation, preserving contents up to min(old, new).
 * krealloc(NULL, n) == kmalloc(n).
 * krealloc(p, 0)   == kfree(p), returns NULL.
 * On failure returns NULL and leaves the original allocation untouched. */
void *krealloc(void *ptr, size_t new_size);

/* Summary: chunks, blocks, bytes used, bytes free, largest free block. */
struct heap_stats {
    size_t chunks;
    size_t blocks;              /* total blocks across all chunks */
    size_t blocks_free;
    size_t bytes_used;          /* payload bytes currently allocated */
    size_t bytes_free;          /* payload bytes in free blocks */
    size_t largest_free;        /* biggest free block, in payload bytes */
};
struct heap_stats heap_get_stats(void);

/* Verify every block header in every chunk.
 * Panics on the first inconsistency. Cheap in the fast build (it is not compiled),
 * so callers can guard with #ifdef or just call it,
 * in the fast build it is a no-op stub that returns immediately. */
void heapcheck(void);

#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <core/ext.h>
#include <core/heap.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/pmm.h>
#include <lib/mem.h>

/*  tunables  */

#define HEAP_CHUNK_SIZE     (256 * 1024)    /* one chunk = 256 KiB */
#define HEAP_ALIGN          16
#define HEAP_MIN_BLOCK_SIZE HEAP_ALIGN      /* smallest splittable payload */

/*  debug scaffolding  */

#ifdef SYM_MEMDEBUG
#define BLOCK_MAGIC     0xC0FFEE5Aull
#define POISON_FREE     0xDD
#define POISON_ALLOC    0xAA
#else
#define BLOCK_MAGIC     0ull    /* not stored; the field exists but is unused */
#endif

/*  block header
 *
 * Every block begins with one of these. The next block's header is at `this + size`.
 * A sentinel block with size == 0 marks the end of a chunk.
 * Payload starts at `header + sizeof(struct block_header)`, rounded up to HEAP_ALIGN.
 * We keep the header size a multiple of HEAP_ALIGN so payload offsets are always aligned. */

struct block_header {
    size_t size;        /* total block size, header + payload, multiple of 16 */
    bool   free;
    uint32_t magic;     /* debug only: BLOCK_MAGIC when the header is valid */
#ifdef SYM_MEMDEBUG
    const char *owner;  /* ext_current() at alloc time, or NULL */
#endif
};

/* Pad the header to a multiple of HEAP_ALIGN so payload is always 16-aligned. */
struct block_header_padded {
    struct block_header h;
    uint8_t pad[HEAP_ALIGN - (sizeof(struct block_header) % HEAP_ALIGN)];
};

#define HDR_SIZE  (sizeof(struct block_header_padded))
#define BLOCK_PAYLOAD(b)  ((uint8_t *)(b) + HDR_SIZE)
#define BLOCK_PAYLOAD_SIZE(b)  ((b)->size - HDR_SIZE)

/*  state  */

struct chunk {
    struct chunk *next;
    uint64_t base_phys;     /* physical address of the chunk, for diagnostics */
    size_t   size;          /* total bytes reserved for this chunk, from base_phys */
};

static struct chunk *chunks_head;

/*  header validation  */

static void header_check(struct block_header *b, const char *where) {
#ifdef SYM_MEMDEBUG
    if (b->magic != (uint32_t)BLOCK_MAGIC) {
        klog_warning("→ SYM_MEMDEBUG: bad magic 0x%x (expected 0x%x) at %p (%s)\n",
                b->magic, (unsigned)BLOCK_MAGIC, (void *)b, where);
        PANIC("heap: block header corruption");
    }
#else
    (void)b; (void)where;
#endif
}

/*  chunk creation  */

static struct chunk *chunk_create(size_t payload_bytes) {
    size_t total = HEAP_CHUNK_SIZE;
    if (payload_bytes + HDR_SIZE + sizeof(struct chunk) > total)
        total = payload_bytes + HDR_SIZE + sizeof(struct chunk) + HEAP_ALIGN;

    size_t frames = (total + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys = pmm_alloc_contig(frames);
    if (phys == 0)
        return NULL;

    uint8_t *base = (uint8_t *)pmm_phys_to_virt(phys);

    struct chunk *c = (struct chunk *)base;
    c->next = NULL;
    c->base_phys = phys;
    /* Record the size actually reserved so heapcheck can bound the chunk
     * exactly, without assuming HEAP_CHUNK_SIZE. */
    c->size = frames * PMM_PAGE_SIZE;

    struct block_header *first =
        (struct block_header *)(base + sizeof(struct chunk));
    size_t first_size = c->size - sizeof(struct chunk);
    first_size &= ~(size_t)(HEAP_ALIGN - 1);

    first->size = first_size;
    first->free = true;
#ifdef SYM_MEMDEBUG
    first->magic = (uint32_t)BLOCK_MAGIC;
    first->owner = NULL;
#endif

    struct block_header *sent = (struct block_header *)((uint8_t *)first + first_size);
    sent->size = 0;
    sent->free = false;
#ifdef SYM_MEMDEBUG
    sent->magic = (uint32_t)BLOCK_MAGIC;
    sent->owner = NULL;
#endif

    c->next = chunks_head;
    chunks_head = c;

    return c;
}

/*  init  */

void heap_init(void) {
    chunks_head = NULL;
    struct chunk *c = chunk_create(0);
    if (c == NULL)
        PANIC("heap: cannot allocate initial chunk (pmm has no frames)");

    struct heap_stats s = heap_get_stats();
    klog("→ heap_init() chunk=%u KiB, chunks=%zu, free=%u KiB\n",
            (unsigned)(HEAP_CHUNK_SIZE / 1024),
            s.chunks, (unsigned)(s.bytes_free / 1024));
}

/*  allocation  */

static void block_split(struct block_header *b, size_t wanted) {
    size_t min_total = wanted + HDR_SIZE;
    if (b->size < min_total + HDR_SIZE + HEAP_MIN_BLOCK_SIZE)
        return;

    size_t new_size = b->size - min_total;
    struct block_header *rest = (struct block_header *)((uint8_t *)b + min_total);
    rest->size = new_size;
    rest->free = true;
#ifdef SYM_MEMDEBUG
    rest->magic = (uint32_t)BLOCK_MAGIC;
    rest->owner = NULL;
#endif

    b->size = min_total;
}

static struct block_header *block_coalesce_next(struct block_header *b) {
    struct block_header *next = (struct block_header *)((uint8_t *)b + b->size);
    if (next->size == 0)
        return b;
    if (!next->free)
        return b;
    b->size += next->size;
    return b;
}

void *kmalloc(size_t size) {
    if (size == 0)
        size = 1;

    /* Reject sizes that would overflow the payload or total arithmetic below.
     * HEAP_ALIGN is a power of two, so size + HEAP_ALIGN - 1 overflows iff
     * size > SIZE_MAX - HEAP_ALIGN + 1, i.e. size > SIZE_MAX - (HEAP_ALIGN - 1). */
    if (size > SIZE_MAX - (HEAP_ALIGN - 1))
        return NULL;

    size_t payload = (size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);
    if (payload > SIZE_MAX - HDR_SIZE)
        return NULL;
    size_t total = payload + HDR_SIZE;

    for (struct chunk *c = chunks_head; c; c = c->next) {
        struct block_header *b =
            (struct block_header *)((uint8_t *)c + sizeof(struct chunk));
        while (b->size != 0) {
            header_check(b, "kmalloc scan");
            if (b->free && b->size >= total) {
                block_split(b, payload);
                b->free = false;
#ifdef SYM_MEMDEBUG
                b->owner = ext_current();
                memset(BLOCK_PAYLOAD(b), POISON_ALLOC, BLOCK_PAYLOAD_SIZE(b));
#endif
                return BLOCK_PAYLOAD(b);
            }
            b = (struct block_header *)((uint8_t *)b + b->size);
        }
    }

    struct chunk *c = chunk_create(total);
    if (c == NULL)
        return NULL;

    struct block_header *b =
        (struct block_header *)((uint8_t *)c + sizeof(struct chunk));
    if (b->size < total) {
        PANIC("heap: new chunk too small for the request");
    }
    block_split(b, payload);
    b->free = false;
#ifdef SYM_MEMDEBUG
    b->owner = ext_current();
    memset(BLOCK_PAYLOAD(b), POISON_ALLOC, BLOCK_PAYLOAD_SIZE(b));
#endif
    return BLOCK_PAYLOAD(b);
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

/*  free  */

void kfree(void *ptr) {
    if (ptr == NULL)
        return;

    struct block_header *b =
        (struct block_header *)((uint8_t *)ptr - HDR_SIZE);

    header_check(b, "kfree");
    if (b->free) {
        klog_error("HEAP: double free of %p (owner at free time: %s)\n",
                ptr,
#ifdef SYM_MEMDEBUG
                b->owner ? b->owner : "(none)"
#else
                "(unknown)"
#endif
        );
        PANIC("heap: double free");
    }

#ifdef SYM_MEMDEBUG
    memset(ptr, POISON_FREE, BLOCK_PAYLOAD_SIZE(b));
    b->owner = NULL;
#endif

    b->free = true;

    b = block_coalesce_next(b);

    for (struct chunk *c = chunks_head; c; c = c->next) {
        uint8_t *chunk_start = (uint8_t *)c + sizeof(struct chunk);
        uint8_t *chunk_end;
        {
            struct block_header *w = (struct block_header *)chunk_start;
            while (w->size != 0)
                w = (struct block_header *)((uint8_t *)w + w->size);
            chunk_end = (uint8_t *)w;
        }
        if ((uint8_t *)b < chunk_start || (uint8_t *)b >= chunk_end)
            continue;

        struct block_header *prev = NULL;
        struct block_header *w = (struct block_header *)chunk_start;
        while ((uint8_t *)w < (uint8_t *)b) {
            prev = w;
            w = (struct block_header *)((uint8_t *)w + w->size);
        }
        if (prev && prev->free) {
            block_coalesce_next(prev);
        }
        return;
    }

    PANIC("heap: kfree of a pointer not in any chunk");
}

/*  realloc  */

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block_header *b =
        (struct block_header *)((uint8_t *)ptr - HDR_SIZE);
    header_check(b, "krealloc");

    size_t old_payload = BLOCK_PAYLOAD_SIZE(b);
    size_t new_payload = (new_size > SIZE_MAX - (HEAP_ALIGN - 1))
                       ? SIZE_MAX
                       : (new_size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);

    if (new_payload <= old_payload) {
        return ptr;
    }

    void *fresh = kmalloc(new_size);
    if (fresh == NULL)
        return NULL;
    memcpy(fresh, ptr, old_payload);
    kfree(ptr);
    return fresh;
}

/*  stats  */

struct heap_stats heap_get_stats(void) {
    struct heap_stats s = {0};

    for (struct chunk *c = chunks_head; c; c = c->next) {
        s.chunks++;
        struct block_header *b =
            (struct block_header *)((uint8_t *)c + sizeof(struct chunk));
        while (b->size != 0) {
            s.blocks++;
            if (b->free) {
                s.blocks_free++;
                s.bytes_free += BLOCK_PAYLOAD_SIZE(b);
                if (BLOCK_PAYLOAD_SIZE(b) > s.largest_free)
                    s.largest_free = BLOCK_PAYLOAD_SIZE(b);
            } else {
                s.bytes_used += BLOCK_PAYLOAD_SIZE(b);
            }
            b = (struct block_header *)((uint8_t *)b + b->size);
        }
    }
    return s;
}

/*  check  */

void heapcheck(void) {
#ifdef SYM_MEMDEBUG
    for (struct chunk *c = chunks_head; c; c = c->next) {
        struct block_header *b =
            (struct block_header *)((uint8_t *)c + sizeof(struct chunk));
        uint8_t *chunk_start = (uint8_t *)c;
        /* Use the recorded chunk size, not HEAP_CHUNK_SIZE, so a chunk
         * grown for a large request is bounded correctly. */
        uint8_t *chunk_end_limit = chunk_start + c->size;
        size_t seen = 0;
        while (b->size != 0) {
            if ((uint8_t *)b < chunk_start || (uint8_t *)b >= chunk_end_limit)
                PANIC("heapcheck: block pointer outside chunk");
            header_check(b, "heapcheck");
            if ((b->size & (HEAP_ALIGN - 1)) != 0)
                PANIC("heapcheck: block size not aligned");
            if (b->size < HDR_SIZE + HEAP_MIN_BLOCK_SIZE)
                PANIC("heapcheck: block smaller than minimum");
            b = (struct block_header *)((uint8_t *)b + b->size);
            if (++seen > 100000)
                PANIC("heapcheck: block chain too long (cycle?)");
        }
        header_check(b, "heapcheck sentinel");
    }
#else
    /* Alternate build; nothing to check beyond the invariants already enforced by kfree and kmalloc.
     * Kept as a no-op so callers do not need to conditionally compile their check calls. */
#endif
}

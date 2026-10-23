#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <core/version.h>
#include <core/arch/pmm.h>
#include <core/arch/heap.h>
#include <core/ext.h>
#include <core/klog.h>
#include <core/panic.h>

// Moved to: config.h
//#define HEAP_CHUNK_SIZE     (256 * 1024)
//#define HEAP_ALIGN          16
//#define HEAP_MIN_BLOCK_SIZE HEAP_ALIGN

#ifdef SYM_MEMDEBUG
#define BLOCK_MAGIC     0xC0FFEE5Aull
#define POISON_FREE     0xDD
#define POISON_ALLOC    0xAA
#else
#define BLOCK_MAGIC     0ull
#endif

struct block_header {
    size_t size;
    bool   free;
    uint32_t magic;
#ifdef SYM_MEMDEBUG
    const char *owner;
#endif
} __attribute__((aligned(HEAP_ALIGN)));

/* Both headers are rounded to HEAP_ALIGN so payload and sentinel stay 16-aligned. */
#define HDR_SIZE        ((sizeof(struct block_header) + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1))
#define CHUNK_HDR_SIZE  ((sizeof(struct chunk) + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1))
#define BLOCK_PAYLOAD(b)       ((uint8_t *)(b) + HDR_SIZE)
#define BLOCK_PAYLOAD_SIZE(b)  ((b)->size - HDR_SIZE)

struct chunk {
    struct chunk *next;
    uint64_t base_phys;
    size_t   size;
};

static struct chunk *chunks_head;

static void header_check(struct block_header *b, const char *where) {
#ifdef SYM_MEMDEBUG
    if (b->magic != (uint32_t)BLOCK_MAGIC) {
        klog_warning("bad magic 0x%x at %p (%s)\n", b->magic, (void *)b, where);
        PANIC("heap: header corruption");
    }
#else
    (void)b; (void)where;
#endif
}

static struct chunk *chunk_create(size_t payload_bytes) {
    if (payload_bytes > SIZE_MAX - HDR_SIZE - CHUNK_HDR_SIZE)
        return NULL;

    size_t need = payload_bytes + HDR_SIZE + CHUNK_HDR_SIZE;
    size_t total = HEAP_CHUNK_SIZE;
    if (need > total)
        total = need + HEAP_ALIGN;

    size_t frames = (total + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys = pmm_alloc_contig(frames);
    if (phys == 0)
        return NULL;

    uint8_t *base = (uint8_t *)pmm_phys_to_virt(phys);

    struct chunk *c = (struct chunk *)base;
    c->base_phys = phys;
    c->size = frames * PMM_PAGE_SIZE;
    c->next = NULL;

    struct block_header *first = (struct block_header *)(base + CHUNK_HDR_SIZE);
    // not good, wastes 16 bytes
    //size_t first_size = (c->size - CHUNK_HDR_SIZE) & ~(size_t)(HEAP_ALIGN - 1);
    size_t first_size = (c->size - CHUNK_HDR_SIZE - HDR_SIZE) & ~(size_t)(HEAP_ALIGN - 1);

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

void heap_init(void) {
    chunks_head = NULL;
    if (chunk_create(0) == NULL)
        PANIC("heap: cannot allocate initial chunk");

    struct heap_stats s = heap_get_stats();
    klog("→ heap_init() chunk=%u KiB, chunks=%zu, free=%u KiB\n",
            (unsigned)(HEAP_CHUNK_SIZE / 1024),
            s.chunks, (unsigned)(s.bytes_free / 1024));
}

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
    if (next->size == 0 || !next->free)
        return b;
    b->size += next->size;
    return b;
}

void *kmalloc(size_t size) {
    if (size == 0)
        size = 1;
    if (size > SIZE_MAX - (HEAP_ALIGN - 1))
        return NULL;

    size_t payload = (size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);
    if (payload > SIZE_MAX - HDR_SIZE - CHUNK_HDR_SIZE)
        return NULL;
    size_t total = payload + HDR_SIZE;

    for (struct chunk *c = chunks_head; c; c = c->next) {
        struct block_header *b = (struct block_header *)((uint8_t *)c + CHUNK_HDR_SIZE);
#ifdef SYM_MEMDEBUG
        size_t seen = 0;
#endif
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
#ifdef SYM_MEMDEBUG
            if (++seen > 100000)
                PANIC("heap: kmalloc scan cycle");
#endif
        }
    }

    struct chunk *c = chunk_create(total);
    if (c == NULL)
        return NULL;

    struct block_header *b = (struct block_header *)((uint8_t *)c + CHUNK_HDR_SIZE);
    if (b->size < total)
        PANIC("heap: new chunk too small");
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

void kfree(void *ptr) {
    if (ptr == NULL)
        return;

    struct block_header *b = (struct block_header *)((uint8_t *)ptr - HDR_SIZE);

    /* 1. Locate: find the owning chunk and the block's predecessor,
     *    and confirm b is a real block boundary. No writes yet. */
    struct block_header *prev = NULL;
    bool found = false;

    for (struct chunk *c = chunks_head; c && !found; c = c->next) {
        uint8_t *start = (uint8_t *)c + CHUNK_HDR_SIZE;
        uint8_t *limit = (uint8_t *)c + c->size - HDR_SIZE;   /* sentinel */
        if ((uint8_t *)b < start || (uint8_t *)b >= limit)
            continue;

        struct block_header *w = (struct block_header *)start;
        prev = NULL;
        while ((uint8_t *)w < (uint8_t *)b) {
            if (w->size == 0)
                PANIC("heap: kfree walk hit sentinel");
            prev = w;
            w = (struct block_header *)((uint8_t *)w + w->size);
        }
        if (w != b)
            PANIC("heap: kfree of a pointer that is not a block");
        found = true;
    }
    if (!found)
        PANIC("heap: kfree of a pointer not in any chunk");

    /* 2. Validate the header. */
    header_check(b, "kfree");
    if (b->free) {
        klog_error("heap: double free of %p\n", ptr);
        PANIC("heap: double free");
    }

    /* 3. Mutate. */
#ifdef SYM_MEMDEBUG
    memset(ptr, POISON_FREE, BLOCK_PAYLOAD_SIZE(b));
    b->owner = NULL;
#endif
    b->free = true;
    block_coalesce_next(b);
    if (prev && prev->free)
        block_coalesce_next(prev);
}


void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block_header *b = (struct block_header *)((uint8_t *)ptr - HDR_SIZE);
    header_check(b, "krealloc");

    size_t old_payload = BLOCK_PAYLOAD_SIZE(b);

    if (new_size > SIZE_MAX - (HEAP_ALIGN - 1))
        return NULL;
    size_t new_payload = (new_size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);

    if (new_payload <= old_payload)
        return ptr;

    void *fresh = kmalloc(new_size);
    if (fresh == NULL)
        return NULL;
    memcpy(fresh, ptr, old_payload);
    kfree(ptr);
    return fresh;
}

struct heap_stats heap_get_stats(void) {
    struct heap_stats s = {0};

    for (struct chunk *c = chunks_head; c; c = c->next) {
        s.chunks++;
        struct block_header *b = (struct block_header *)((uint8_t *)c + CHUNK_HDR_SIZE);
        while (b->size != 0) {
            s.blocks++;
            size_t ps = BLOCK_PAYLOAD_SIZE(b);
            if (b->free) {
                s.blocks_free++;
                s.bytes_free += ps;
                if (ps > s.largest_free)
                    s.largest_free = ps;
            } else {
                s.bytes_used += ps;
            }
            b = (struct block_header *)((uint8_t *)b + b->size);
        }
    }
    return s;
}

void heapcheck(void) {
#ifdef SYM_MEMDEBUG
    for (struct chunk *c = chunks_head; c; c = c->next) {
        uint8_t *chunk_start = (uint8_t *)c;
        uint8_t *chunk_limit = chunk_start + c->size;
        struct block_header *b = (struct block_header *)(chunk_start + CHUNK_HDR_SIZE);
        size_t seen = 0;

        while (b->size != 0) {
            if ((uint8_t *)b < chunk_start || (uint8_t *)b >= chunk_limit)
                PANIC("heapcheck: block outside chunk");
            header_check(b, "heapcheck");
            if ((b->size & (HEAP_ALIGN - 1)) != 0)
                PANIC("heapcheck: block size misaligned");
            if (b->size < HDR_SIZE + HEAP_MIN_BLOCK_SIZE)
                PANIC("heapcheck: block below minimum");
            b = (struct block_header *)((uint8_t *)b + b->size);
            if (++seen > 100000)
                PANIC("heapcheck: block chain cycle");
        }
        header_check(b, "heapcheck sentinel");
    }
#endif
}

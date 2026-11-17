#include <stddef.h>
#include <lib/string.h>
#include <lib/mem.h>
#include <core/arch/boot.h>
#include <core/arch/pmm.h>
#include <core/arch/heap.h>
#include <core/arch/vmm.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/kprintf.h>

#include <ext/testers/memtest.h>

// codebase: 0.2.0

/*  memtest  */

static int test_pmm_distinct(void) {
    enum { N = 16 };
    uint64_t frames[N];
    for (int i = 0; i < N; i++) {
        frames[i] = pmm_alloc();
        if (frames[i] == 0) {
            for (int j = 0; j < i; j++) pmm_free(frames[j], 1);
            kprintf("pmm_distinct : FAIL (ran out at %d)\n", i);
            return 1;
        }
    }
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            if (frames[i] == frames[j]) {
                kprintf("pmm_distinct : FAIL (duplicate 0x%llx)\n",
                        (unsigned long long)frames[i]);
                for (int k = 0; k < N; k++) pmm_free(frames[k], 1);
                return 1;
            }
        }
    }
    for (int i = 0; i < N; i++)
        pmm_free(frames[i], 1);
    kprintf("pmm_distinct : ok\n");
    return 0;
}

static int test_pmm_align(void) {
    enum { N = 16 };
    uint64_t frames[N];
    for (int i = 0; i < N; i++) {
        frames[i] = pmm_alloc();
        if (frames[i] == 0) {
            for (int j = 0; j < i; j++) pmm_free(frames[j], 1);
            kprintf("pmm_align    : SKIP (ran out at %d)\n", i);
            return 0;
        }
        if ((frames[i] & (PMM_PAGE_SIZE - 1)) != 0) {
            kprintf("pmm_align    : FAIL (0x%llx not page-aligned)\n",
                    (unsigned long long)frames[i]);
            for (int k = 0; k <= i; k++) pmm_free(frames[k], 1);
            return 1;
        }
    }
    for (int i = 0; i < N; i++)
        pmm_free(frames[i], 1);
    kprintf("pmm_align    : ok\n");
    return 0;
}

/* Built-in memory testers  */

static int test_pmm_contig(void) {
    enum { N = 8 };

    struct pmm_stats before = pmm_get_stats();

    uint64_t base = pmm_alloc_contig(N);
    if (base == 0) {
        kprintf("pmm_contig   : SKIP (no %d-frame run available)\n", N);
        return 0;
    }
    if ((base & (PMM_PAGE_SIZE - 1)) != 0) {
        kprintf("pmm_contig   : FAIL (base 0x%llx not page-aligned)\n",
                (unsigned long long)base);
        pmm_free(base, N);
        return 1;
    }

    pmm_free(base, N);

    uint64_t again = pmm_alloc_contig(N);
    if (again == 0) {
        kprintf("pmm_contig   : FAIL (re-alloc failed)\n");
        return 1;
    }
    if ((again & (PMM_PAGE_SIZE - 1)) != 0) {
        kprintf("pmm_contig   : FAIL (re-alloc 0x%llx not page-aligned)\n",
                (unsigned long long)again);
        pmm_free(again, N);
        return 1;
    }
    pmm_free(again, N);

    struct pmm_stats after = pmm_get_stats();
    if (after.free_frames != before.free_frames) {
        kprintf("pmm_contig   : FAIL (free count %zu -> %zu)\n",
                before.free_frames, after.free_frames);
        return 1;
    }

    kprintf("pmm_contig   : ok (base=0x%llx, realloc=0x%llx)\n",
            (unsigned long long)base, (unsigned long long)again);
    return 0;
}

static int test_pmm_roundtrip(void) {
    struct pmm_stats before = pmm_get_stats();
    size_t allocated = 0;

    size_t cap = before.free_frames;
    if (cap > 4096)
        cap = 4096;

    uint64_t head = 0;
    for (size_t i = 0; i < cap; i++) {
        uint64_t p = pmm_alloc();
        if (p == 0)
            break;

        if ((p & (PMM_PAGE_SIZE - 1)) != 0) {
            kprintf("pmm_roundtrip: FAIL (misaligned frame 0x%llx)\n",
                    (unsigned long long)p);
            return 1;
        }
        size_t frame = (size_t)(p / PMM_PAGE_SIZE);
        if (frame >= before.total_frames) {
            kprintf("pmm_roundtrip: FAIL (out-of-range frame 0x%llx)\n",
                    (unsigned long long)p);
            return 1;
        }

        uint64_t *slot = (uint64_t *)pmm_phys_to_virt(p);
        *slot = head;
        head = p;
        allocated++;
    }

    if (allocated == 0) {
        kprintf("pmm_roundtrip: FAIL (could not allocate any frames)\n");
        return 1;
    }

    size_t freed = 0;
    uint64_t node = head;
    while (node != 0) {
        uint64_t *slot = (uint64_t *)pmm_phys_to_virt(node);
        uint64_t next = *slot;
        pmm_free(node, 1);
        freed++;
        node = next;
    }

    struct pmm_stats after = pmm_get_stats();
    if (after.free_frames != before.free_frames) {
        kprintf("pmm_roundtrip: FAIL (before=%zu after=%zu)\n",
                before.free_frames, after.free_frames);
        return 1;
    }

    kprintf("pmm_roundtrip: ok (%zu frames)\n", freed);
    return 0;
}

static int test_heap_zero(void) {
    void *p = kzalloc(64);
    if (!p) {
        kprintf("heap_zero    : SKIP (kzalloc failed)\n");
        return 0;
    }
    uint8_t *bytes = p;
    for (int i = 0; i < 64; i++) {
        if (bytes[i] != 0) {
            kprintf("heap_zero    : FAIL (byte %d = 0x%02x)\n", i, bytes[i]);
            kfree(p);
            return 1;
        }
    }
    kfree(p);
    kprintf("heap_zero    : ok\n");
    return 0;
}

static int test_heap_null(void) {
    kfree(NULL);
    kprintf("heap_null    : ok\n");
    return 0;
}

static int test_heap_realloc(void) {
    char *p = kmalloc(16);
    if (!p) {
        kprintf("heap_realloc : SKIP\n");
        return 0;
    }
    memcpy(p, "0123456789abcdef", 16);
    char *q = krealloc(p, 32);
    if (!q) {
        kprintf("heap_realloc : FAIL (grow returned NULL)\n");
        kfree(p);
        return 1;
    }
    if (memcmp(q, "0123456789abcdef", 16) != 0) {
        kprintf("heap_realloc : FAIL (contents lost on grow)\n");
        kfree(q);
        return 1;
    }
    char *r = krealloc(q, 8);
    if (!r) {
        kprintf("heap_realloc : FAIL (shrink returned NULL)\n");
        kfree(q);
        return 1;
    }
    if (memcmp(r, "01234567", 8) != 0) {
        kprintf("heap_realloc : FAIL (contents lost on shrink)\n");
        kfree(r);
        return 1;
    }
    kfree(r);
    kprintf("heap_realloc : ok\n");
    return 0;
}

static int test_vmm_map_unmap(void) {
    uint64_t va = VMM_DYNAMIC_VMTEST;
    if (vmm_translate(va) != 0) {
        kprintf("vmm_map_unmap: SKIP (VMM_DYNAMIC_VMTEST already mapped)\n");
        return 0;
    }

    uint64_t frame = pmm_alloc();
    if (frame == 0) {
        kprintf("vmm_map_unmap: SKIP\n");
        return 0;
    }
    if (!vmm_map(va, frame, VMM_WRITE | VMM_NX)) {
        kprintf("vmm_map_unmap: FAIL (map returned false)\n");
        pmm_free(frame, 1);
        return 1;
    }
    if (vmm_translate(va) != frame) {
        kprintf("vmm_map_unmap: FAIL (translate != frame)\n");
        vmm_unmap(va);
        pmm_free(frame, 1);
        return 1;
    }
    if (!vmm_unmap(va)) {
        kprintf("vmm_map_unmap: FAIL (unmap returned false)\n");
        pmm_free(frame, 1);
        return 1;
    }
    if (vmm_translate(va) != 0) {
        kprintf("vmm_map_unmap: FAIL (still mapped after unmap)\n");
        pmm_free(frame, 1);
        return 1;
    }
    pmm_free(frame, 1);
    kprintf("vmm_map_unmap: ok\n");
    return 0;
}

int cmd_memtest_fn(const struct cmd_args *a) {
    (void)a;

    struct test {
        const char *name;
        int (*fn)(void);
    };
    static const struct test tests[] = {
        { "pmm_distinct",    test_pmm_distinct },
        { "pmm_align",       test_pmm_align },
        { "pmm_contig",      test_pmm_contig },
        { "pmm_roundtrip",   test_pmm_roundtrip },
        { "heap_zero",       test_heap_zero },
        { "heap_null",       test_heap_null },
        { "heap_realloc",    test_heap_realloc },
        { "vmm_map_unmap",   test_vmm_map_unmap },
    };

    size_t ran = 0, failed = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ran++;
        if (tests[i].fn() != 0)
            failed++;
    }

    kprintf("Memtest: %zu ran, %zu failed\n", ran, failed);
    return failed ? 1 : CMD_OK;
}

/*  Heap tester  */

int cmd_heaptest_fn(const struct cmd_args *a) {
    unsigned iterations = 2000;
    if (a->argc >= 1)
        iterations = (unsigned)a->num[0];
    if (iterations > 100000)
        iterations = 100000;

    enum { N = 64 };
    void *ptrs[N] = {0};
    size_t sizes[N] = {0};

    uint32_t seed = 0x12345678;
    struct heap_stats before = heap_get_stats();

    for (unsigned iter = 0; iter < iterations; iter++) {
        seed = seed * 1103515245u + 12345u;

        int slot = (int)((seed >> 16) % N);
        if (ptrs[slot] == NULL) {
            size_t sz = 1 + ((seed >> 8) % 1024);
            void *p = kmalloc(sz);
            if (p == NULL)
                continue;
            uint8_t *bytes = p;
            for (size_t i = 0; i < sz; i++)
                bytes[i] = (uint8_t)(slot ^ i);
            ptrs[slot] = p;
            sizes[slot] = sz;
        } else {
            uint8_t *bytes = ptrs[slot];
            for (size_t i = 0; i < sizes[slot]; i++) {
                if (bytes[i] != (uint8_t)(slot ^ i)) {
                    kprintf("Heaptest: corruption in slot %d at byte %zu\n",
                            slot, i);
                    return 1;
                }
            }
            kfree(ptrs[slot]);
            ptrs[slot] = NULL;
            sizes[slot] = 0;
        }
    }

    for (int i = 0; i < N; i++) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }
    }

    struct heap_stats after = heap_get_stats();

    size_t live_before = before.blocks - before.blocks_free;
    size_t live_after  = after.blocks  - after.blocks_free;

    if (after.bytes_used != before.bytes_used ||
        live_after != live_before ||
        after.blocks != before.blocks ||
        after.blocks_free != before.blocks_free) {
        kprintf("Heaptest: leak: used %zu -> %zu bytes, live blocks %zu -> %zu\n",
                before.bytes_used, after.bytes_used, live_before, live_after);
        return 1;
    }

    kprintf("Heaptest: %u iterations OK (chunks: %zu -> %zu, used: %zu bytes)\n",
            iterations, before.chunks, after.chunks, after.bytes_used);
    return CMD_OK;
}

SYM_COMMAND(heaptest, "[iterations:num]", "Perform heap test", cmd_heaptest_fn);

/* extension greeter */
//static int memory_ext_init(void) { return 0; }
//SYM_EXTENSION(mem_toolkit, memory_ext_init, EXT_PRIO_APPLET);

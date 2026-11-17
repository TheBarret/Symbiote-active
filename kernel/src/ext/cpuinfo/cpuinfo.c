#include <stdbool.h>
#include <core/arch/cpu.h>
#include <core/cmd.h>
#include <core/kprintf.h>
#include <core/ext.h>

/* CPU identity and features via CPUID. */

struct cpu_info {
    char     vendor[13];    /* "GenuineIntel" or "AuthenticAMD" */
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t max_basic_leaf;
    uint32_t max_extended_leaf;
    /* Feature flags, one bit each. */
    bool     sse;
    bool     sse2;
    bool     sse3;
    bool     ssse3;
    bool     sse41;
    bool     sse42;
    bool     avx;
    bool     avx2;
    bool     rdrand;
    bool     rdseed;
    bool     xsave;
    /* ... add more as they matter ... */
};

static struct cpu_info cpu;

static void cpu_scan(void) {
    uint32_t a, b, c, d;

    /* Leaf 0: max basic leaf and vendor string. */
    cpuid(0, 0, &a, &b, &c, &d);
    cpu.max_basic_leaf = a;
    *(uint32_t *)&cpu.vendor[0] = b;
    *(uint32_t *)&cpu.vendor[4] = d;
    *(uint32_t *)&cpu.vendor[8] = c;
    cpu.vendor[12] = '\0';

    /* Leaf 1: family/model/stepping and feature flags (SSE, SSE2, etc.). */
    cpuid(1, 0, &a, &b, &c, &d);
    cpu.stepping = a & 0xF;
    cpu.model    = (a >> 4) & 0xF;
    cpu.family   = (a >> 8) & 0xF;
    cpu.sse      = (d >> 25) & 1;
    cpu.sse2     = (d >> 26) & 1;
    cpu.sse3     = (c >> 0) & 1;
    cpu.ssse3    = (c >> 9) & 1;
    cpu.sse41    = (c >> 19) & 1;
    cpu.sse42    = (c >> 20) & 1;
    cpu.avx      = (c >> 28) & 1;
    cpu.xsave    = (c >> 26) & 1;

    /* Leaf 7: extended features (AVX2, RDRAND on some parts, RDSEED, ...). */
    if (cpu.max_basic_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        cpu.avx2   = (b >> 5) & 1;
        cpu.rdseed = (b >> 18) & 1;
    }

    /* Extended leaf 0x80000000: max extended leaf. */
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    cpu.max_extended_leaf = a;

    /* Extended leaf 0x80000001: RDRAND and others. */
    if (cpu.max_extended_leaf >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        cpu.rdrand = (c >> 30) & 1;
    }
}

static int cmd_cpu_fn(const struct cmd_args *a) {
    (void)a;

    kprintf("CPU: %s family %u model %u stepping %u (", cpu.vendor, cpu.family, cpu.model, cpu.stepping);
    if (cpu.sse)   kprintf(" SSE");
    if (cpu.sse2)  kprintf(" SSE2");
    if (cpu.sse3)  kprintf(" SSE3");
    if (cpu.ssse3) kprintf(" SSSE3");
    if (cpu.sse41) kprintf(" SSE4.1");
    if (cpu.sse42) kprintf(" SSE4.2");
    if (cpu.avx)   kprintf(" AVX");
    if (cpu.avx2)  kprintf(" AVX2");
    if (cpu.rdrand) kprintf(" RDRAND");
    if (cpu.rdseed) kprintf(" RDSEED");
    kprintf(" )\n");
    return CMD_OK;
}

SYM_COMMAND(cpuinfo, "", "Display CPU info", cmd_cpu_fn);

static int cpu_ext_init(void) {
    cpu_scan();
    return 0;
}

SYM_EXTENSION(cpuinfo, cpu_ext_init, EXT_PRIO_APPLET);

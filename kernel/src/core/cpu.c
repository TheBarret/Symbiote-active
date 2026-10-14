#include <stdint.h>
#include <core/cpu.h>

/* CPUID leaf 0x15 reports the ratio of the TSC frequency to the core
 * crystal clock:
 *     EBX = TSC numerator   (TSC ticks per crystal tick)
 *     EAX = TSC denominator
 *     ECX = crystal Hz, or 0 if the CPU does not report it
 * TSC_Hz = (EBX / EAX) * ECX.
 * When ECX is 0, the crystal frequency has to come from the
 * family/model table below. */

/* Nominal core crystal clock for common Intel families, in Hz.
 * Values from the Intel SDM's table for leaf 0x15 when ECX is 0. */
static uint64_t crystal_hz_for(uint32_t family, uint32_t model) {
    /* TODO: define more options. */
    switch (family) {
    case 0x06:
        switch (model) {
        case 0x4E: case 0x5E:                       /* Skylake, Kaby Lake */
        case 0x8E: case 0x9E:                       /* Coffee Lake        */
        case 0xA5: case 0xA6:                       /* Comet Lake         */
            return 24000000;
        case 0x55:                                  /* Cascade Lake       */
            return 25000000;
        case 0x5C: case 0x5F:                       /* Goldmont, Denverton*/
            return 19200000;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

uint64_t cpu_tsc_hz(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Check the maximum CPUID leaf before probing 0x15. */
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax < 0x15)
        return 0;

    cpuid(0x15, 0, &eax, &ebx, &ecx, &edx);
    if (eax == 0 || ebx == 0)
        return 0;   /* CPU does not report the ratio */

    uint64_t crystal = ecx;
    if (crystal == 0) {
        /* Fall back to the family/model table. */
        uint32_t f_eax, f_ebx, f_ecx, f_edx;
        cpuid(1, 0, &f_eax, &f_ebx, &f_ecx, &f_edx);
        uint32_t family = ((f_eax >> 8) & 0xF) + ((f_eax >> 20) & 0xFF);
        uint32_t model  = ((f_eax >> 4) & 0xF) | (((f_eax >> 16) & 0xF) << 4);
        crystal = crystal_hz_for(family, model);
        if (crystal == 0)
            return 0;
    }

    return (uint64_t)ebx * crystal / eax;
}

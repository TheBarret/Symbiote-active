#include <stdint.h>
#include <core/arch/cpu.h>

/* CPUID leaf 0x15 reports the ratio of the TSC frequency to the core
 * crystal clock:
 *     EBX = TSC numerator   (TSC ticks per crystal tick)
 *     EAX = TSC denominator
 *     ECX = crystal Hz, or 0 if the CPU does not report it
 * TSC_Hz = (EBX / EAX) * ECX.
 * When ECX is 0, the crystal frequency has to come from the
 * family/model table below. */

// codebase: 0.1.2

/* Nominal core crystal clock for common Intel families, in Hz.
 * Values from the Intel SDM's table for leaf 0x15 when ECX is 0.
 * Returns 0 for every AMD CPU today; klog_init falls back to 2.5 GHz.
 * If AMD support matters later, add family 0x17/0x19 (and fix the
 * family/model decode above for base_family == 0xF). */
static uint64_t crystal_hz_for(uint32_t family, uint32_t model) {
    switch (family) {
    case 0x17: /* AMD Zen 1, Zen+, Zen 2 */
    case 0x19: /* AMD Zen 3, Zen 4       */
        /* AMD processors typically do not populate ECX via CPUID 0x15 in the same manner as Intel.
         * If falling back or mapping baseline crystal/REFCLK assumptions for internal calculations.
         */
        return 2500000000ULL;                    /* Fallback to 2.5 GHz nominal TSC */
    case 0x06:
        switch (model) {
            case 0x4E: case 0x5E:                    /* Skylake, Kaby Lake */
            case 0x8E: case 0x9E:                    /* Coffee Lake        */
            case 0xA5: case 0xA6:                    /* Comet Lake         */
            case 0x66:                               /* Cannon Lake        */
            case 0x7D: case 0x7E:                    /* Ice Lake           */
            case 0x8C: case 0x8D:                    /* Tiger Lake         */
            case 0x9A:                               /* Alder Lake Mobile  */
            case 0x97: case 0x98:                    /* Alder Lake Desktop */
            case 0xB7: case 0xBF:                    /* Raptor Lake        */
            case 0xAA: case 0xAC:                    /* Meteor Lake        */
            case 0xBA:                               /* Arrow Lake         */
            case 0xBD:                               /* Lunar Lake         */
                return 24000000;                     /* 24.0 MHz           */
            case 0x55:                               /* Skylake-X, Cascade Lake-X */
            case 0x6A: case 0x6C:                    /* Cooper Lake / Ice Lake-SP */
            case 0x8F:                               /* Sapphire Rapids    */
            case 0xCF:                               /* Emerald Rapids     */
            case 0xAE:                               /* Granite Rapids     */
                return 25000000;                     /* 25.0 MHz           */
            case 0x5C: case 0x5F:                    /* Goldmont (Apollo Lake), Denverton */
            case 0x7A:                               /* Goldmont Plus (Gemini Lake) */
                return 19200000;                     /* 19.2 MHz           */
            case 0x86:                               /* Tremont (Elkhart Lake) */
            case 0x96:                               /* Tremont (Jasper Lake)  */
                return 25000000;                     /* 25.0 MHz (or 19.2 depending on platform) */

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
        uint32_t f_eax, f_ebx, f_ecx, f_edx;
        cpuid(1, 0, &f_eax, &f_ebx, &f_ecx, &f_edx);

        uint32_t base_family = (f_eax >> 8) & 0xF;
        uint32_t ext_family  = (f_eax >> 20) & 0xFF;
        uint32_t base_model  = (f_eax >> 4) & 0xF;
        uint32_t ext_model   = (f_eax >> 16) & 0xF;

        uint32_t family = base_family;
        uint32_t model  = base_model;

        if (base_family == 0xF) {
            family += ext_family;
        }
        if (base_family == 0x6 || base_family == 0xF) {
            model += (ext_model << 4);
        }

        crystal = crystal_hz_for(family, model);
        if (crystal == 0)
            return 0;
    }

    return (uint64_t)ebx * crystal / eax;
}

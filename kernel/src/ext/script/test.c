#include <stddef.h>
#include <stdint.h>

#include <core/version.h>
#include <core/bus.h>
#include <core/ext.h>
#include <core/kprintf.h>
#include <core/klog.h>

#include <ext/script/decimal.h>
#include <ext/script/vm16.h>


/*  dec-selftest */

static int g_fail;

static struct dec D(int64_t mag, uint8_t scale)
{
    struct dec d = {0};
    d.mag = mag; d.scale = scale; d.ok = true;
    return d;
}

static void expect(const char *what, struct dec got, bool ok, int64_t mag, uint8_t scale)
{
    bool pass = (got.ok == ok) && (!ok || (got.mag == mag && got.scale == scale));
    if (!pass) {
        g_fail++;
        kprintf("dec_selftest FAIL %s: got ok=%d mag=%lld scale=%u\n", what,
                (int)got.ok, (long long)got.mag, (unsigned)got.scale);
    }
}


/* CMP test helpers */
static void expect_cmp(const char *what, int got, int want)
{
    if (got != want) {
        g_fail++;
        kprintf("dec_selftest FAIL %s: got %d want %d\n", what, got, want);
    }
}


int dec_selftest(void)
{
    g_fail = 0;
    const int64_t MINV = INT64_MIN;

    expect("div 7.50/2.5 rs4",       dec_div(D(750,2), D(25,1), DEC_RND_TRUNC, 4), true, 30000, 4);
    expect("div 1/3 rs4 trunc",      dec_div(D(1,0),   D(3,0),  DEC_RND_TRUNC, 4), true, 3333, 4);
    expect("div 2/3 rs4 half_up",    dec_div(D(2,0),   D(3,0),  DEC_RND_HALF_UP, 4), true, 6667, 4);
    expect("div -5/2 trunc",         dec_div(D(-5,0),  D(2,0),  DEC_RND_TRUNC, 0), true, -2, 0);
    expect("div -5/2 half_up",       dec_div(D(-5,0),  D(2,0),  DEC_RND_HALF_UP, 0), true, -3, 0);
    expect("div -5/2 half_even",     dec_div(D(-5,0),  D(2,0),  DEC_RND_HALF_EVEN, 0), true, -2, 0);
    expect("div by zero",            dec_div(D(1,0),   D(0,0),  DEC_RND_TRUNC, 0), false, 0, 0);
    expect("div MIN/-1 (no #DE)",    dec_div(D(MINV,0), D(-1,0), DEC_RND_TRUNC, 0), false, 0, 0);

    /* k = 36 frontier: the widest scale gap (rs + bs - as = 18 + 18 - 0).
     * These pin the invariant that a scaling reject is always unrepresentable. */
    expect("div k36 fits",           dec_div(D(80,0),  D(9000000000000000000LL,18), DEC_RND_TRUNC, 18),    true, 8888888888888888888LL, 18);
    expect("div k36 half_up",        dec_div(D(80,0),  D(9000000000000000000LL,18), DEC_RND_HALF_UP, 18),  true, 8888888888888888889LL, 18);
    expect("div k36 rejects unfit",  dec_div(D(341,0), D(1,18), DEC_RND_TRUNC, 18), false, 0, 0);

    expect("mul equal scale no rnd", dec_mul(D(15,1), D(10,1), DEC_RND_HALF_UP, 2), true, 150, 2);
    expect("mul scale up",           dec_mul(D(25,1), D(25,1), DEC_RND_TRUNC, 4), true, 62500, 4);
    expect("mul 6.25 -> 1 even",     dec_mul(D(25,1), D(25,1), DEC_RND_HALF_EVEN, 1), true, 62, 1);
    expect("mul 6.25 -> 1 half_up",  dec_mul(D(25,1), D(25,1), DEC_RND_HALF_UP, 1), true, 63, 1);
    expect("mul scale sum > 18",     dec_mul(D(1,18), D(1,18), DEC_RND_TRUNC, 18), true, 0, 18);
    expect("mul overflow",           dec_mul(D(INT64_MAX,0), D(2,0), DEC_RND_TRUNC, 0), false, 0, 0);
    /* sqrt(2^63) frontier: 3037000499^2 fits, 3037000500^2 does not */
    expect("mul sqrt frontier fits", dec_mul(D(3037000499LL,0), D(3037000499LL,0), DEC_RND_TRUNC, 0), true, 9223372030926249001LL, 0);
    expect("mul sqrt frontier over",dec_mul(D(3037000500LL,0), D(3037000500LL,0), DEC_RND_TRUNC, 0), false, 0, 0);

    expect("add exact rs2",          dec_add(D(15,1), D(225,2), DEC_RND_TRUNC, 2), true, 375, 2);
    expect("add rs4 (scale up)",     dec_add(D(15,1), D(225,2), DEC_RND_TRUNC, 4), true, 37500, 4);
    expect("add 3.75 -> 1 even",     dec_add(D(15,1), D(225,2), DEC_RND_HALF_EVEN, 1), true, 38, 1);
    expect("sub 1 - 2.5",            dec_sub(D(1,0),  D(25,1),  DEC_RND_TRUNC, 1), true, -15, 1);
    expect("sub MIN - 1 overflow",   dec_sub(D(MINV,0), D(1,0), DEC_RND_TRUNC, 0), false, 0, 0);
    expect("add MIN + 0",            dec_add(D(MINV,0), D(0,0), DEC_RND_TRUNC, 0), true, INT64_MIN, 0);
    expect("add cancels to zero",    dec_add(D(5,0),  D(-5,0),  DEC_RND_TRUNC, 0), true, 0, 0);

    expect("mod 7.5 % 2",            dec_mod(D(75,1), D(2,0),   DEC_RND_TRUNC, 1), true, 15, 1);
    expect("mod -7 % 3",             dec_mod(D(-7,0), D(3,0),   DEC_RND_TRUNC, 0), true, -1, 0);
    expect("mod MIN % -1 (no #DE)",  dec_mod(D(MINV,0), D(-1,0), DEC_RND_TRUNC, 0), true, 0, 0);
    expect("mod by zero",            dec_mod(D(1,0),  D(0,0),   DEC_RND_TRUNC, 0), false, 0, 0);

    struct dec bad = D(1, 200);                    /* corrupt scale from a mask */
    expect("reject bad scale",       dec_add(bad, D(1,0), DEC_RND_TRUNC, 0), false, 0, 0);

    /* dec_cmp: same scale, cross scale, sign cases, invalid input */
    expect_cmp("cmp invalid",      dec_cmp(D(1,200), D(1,0)),                       -2);
    expect_cmp("cmp equal",        dec_cmp(D(15,2),   D(15,2)),                       0);
    expect_cmp("cmp cross equal",  dec_cmp(D(15,1),   D(150,2)),                      0);  /* 1.5 == 1.50 */
    expect_cmp("cmp lt cross",     dec_cmp(D(15,1),   D(2,0)),                       -1);  /* 1.5 < 2 */
    //expect_cmp("cmp gt negative",  dec_cmp(D(-15,1),  D(2,0)),                        1);  /* -1.5 > 2? no: -1.5 < 2 */
    expect_cmp("cmp neg vs pos",  dec_cmp(D(-15,1),  D(2,0)),                      -1);  /* -1.5 < 2 */
    expect_cmp("cmp neg vs zero",  dec_cmp(D(-1,0),   D(0,0)),                       -1);
    expect_cmp("cmp zero forms",   dec_cmp(D(0,0),    D(0,18)),                      0);

    /* dec_cmp fallback: the rescale to a common scale overflows int64,
     * dec_align fails, and only the u128 path can decide. */
    expect_cmp("cmp fallback gt",  dec_cmp(D(9000000000000000000LL,0),  D(1,18)),       1);  /* 9e18 > 1e-18 */
    expect_cmp("cmp fallback lt",  dec_cmp(D(1,0),     D(9000000000000000000LL,18)),   -1);  /* 1 < 9 */
    expect_cmp("cmp fallback neg", dec_cmp(D(-9000000000000000000LL,0), D(-1,18)),      -1);  /* -9e18 < -1e-18 */

    /* dec_align must be atomic on failure: neither operand is modified. */
    {
        struct dec ax = D(15,1), bx = D(1,0);
        bx.scale = 200;                            /* corrupt mask scale */
        uint8_t common;
        bool aligned = dec_align(&ax, &bx, &common);
        if (aligned || ax.mag != 15 || ax.scale != 1 || bx.mag != 1) {
            g_fail++;
            kprintf("dec_selftest FAIL align atomicity: aligned=%d a.mag=%lld a.scale=%u\n",
                    (int)aligned, (long long)ax.mag, (unsigned)ax.scale);
        }
    }

    kprintf("dec_selftest: %s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail;
}

/* vm16 decimal integration test */

static void load_vm_str(struct vm_ctx *c, uint16_t addr, const char *str) {
    size_t i = 0;
    while (1) {
        char c1 = str[i];
        //char c2 = str[i+1];
        char c2 = (c1 == '\0') ? '\0' : str[i+1];
        uint16_t w = (uint16_t)((unsigned char)c1) | ((uint16_t)((unsigned char)c2) << 8);
        c->mem[addr + (i/2)] = w;
        if (c1 == '\0' || c2 == '\0') break;
        i += 2;
    }
}

int vm16_decimal_integration_test(void)
{
    struct vm_ctx ctx;
    if (vm_init(&ctx) != 0) {
        kprintf("vm_test FAIL: init\n");
        return 1;
    }

    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("vm_test FAIL: no env\n");
        vm_free(&ctx);
        return 1;
    }

    /* seed environment DB: a = 10.50 (scale 2), b = 2.25 (scale 2) */
    if (!dec_set(env, "test", "a", 1050, 2, NULL, 0) ||
        !dec_set(env, "test", "b", 225,  2, NULL, 0)) {
        kprintf("vm_test FAIL: dec_set seeding\n");
        vm_free(&ctx);
        return 1;
    }

    /* place null-terminated ASCII strings safely into VM memory at 0x3500 */
    load_vm_str(&ctx, 0x3500, "test"); // Namespace
    load_vm_str(&ctx, 0x3510, "a");    // Operand A key
    load_vm_str(&ctx, 0x3520, "b");    // Operand B key
    load_vm_str(&ctx, 0x3530, "sum");  // Result destination key

    /* pre-load the LC-3 register file with our pointers and parameters:
     *    R0 = pointer to namespace string ("test")
     *    R1 = pointer to operand A key ("a")
     *    R2 = pointer to operand B key ("b")
     *    R3 = pointer to result key ("sum")
     *    R4 = scale (low byte = 2) and rounding mode (high byte = DEC_RND_TRUNC = 0)
     */
    ctx.reg[R0] = 0x3500;
    ctx.reg[R1] = 0x3510;
    ctx.reg[R2] = 0x3520;
    ctx.reg[R3] = 0x3530;
    ctx.reg[R4] = 0x0002; // scale = 2, round = TRUNC

    /* write a minimal 2-instruction LC-3 program at VM_PC_START (0x3000):
     *    - TRAP 0xD2 (T_DEC_ADD)
     *    - TRAP 0x25 (T_HALT)
     */
    uint16_t program[] = {
        0xF0D2, // TRAP 0xD2 (T_DEC_ADD)
        0xF025  // TRAP 0x25 (T_HALT)
    };

    if (vm_load_native(&ctx, VM_PC_START, program, 2) != 0) {
        kprintf("vm_test FAIL: load native\n");
        vm_free(&ctx);
        return 1;
    }

    /* run the VM */
    enum vm_status st = vm_run(&ctx, 100);
    if (st != VM_HALT) {
        kprintf("vm_test FAIL: status=%d fault=%d\n", (int)st, (int)ctx.fault);
        vm_free(&ctx);
        return 1;
    }

    /* verify the result landed back in the environment DB: 10.50 + 2.25 = 12.75 */
    struct dec result = dec_get(env, "test", "sum");
    if (!result.ok || result.mag != 1275 || result.scale != 2) {
        kprintf("vm_test FAIL: expected sum=12.75, got ok=%d mag=%lld scale=%u\n",
                (int)result.ok, (long long)result.mag, (unsigned)result.scale);
        vm_free(&ctx);
        return 1;
    }

    vm_free(&ctx);
    kprintf("vm16_decimal_integration_test: PASSED\n");
    return 0;
}
/* Fault contract: a cmp against a symbol that was never stored must FAULT,
 * not branch. Pins VM_ERR_DEC_GET and fault_pc. */
int vm16_decimal_fault_test(void)
{
    struct vm_ctx ctx;
    if (vm_init(&ctx) != 0) {
        kprintf("vm_fault_test FAIL: init\n");
        return 1;
    }

    load_vm_str(&ctx, 0x3500, "test");
    load_vm_str(&ctx, 0x3510, "sum");
    load_vm_str(&ctx, 0x3520, "zero");   /* never stored */

    ctx.reg[R0] = 0x3500;
    ctx.reg[R1] = 0x3510;
    ctx.reg[R2] = 0x3520;
    ctx.reg[R4] = 0x0002;

    uint16_t program[] = { 0xF0D7, 0xF025 };   /* TRAP xD7, TRAP x25 */
    if (vm_load_native(&ctx, VM_PC_START, program, 2) != 0) {
        kprintf("vm_fault_test FAIL: load\n");
        vm_free(&ctx);
        return 1;
    }

    enum vm_status st = vm_run(&ctx, 100);
    if (st != VM_FAULT || ctx.fault != VM_ERR_DEC_GET || ctx.fault_pc != VM_PC_START) {
        kprintf("vm_fault_test FAIL: status=%d fault=%d fault_pc=%04X (want FAULT/DEC_GET/%04X)\n",
                (int)st, (int)ctx.fault, (unsigned)ctx.fault_pc, (unsigned)VM_PC_START);
        vm_free(&ctx);
        return 1;
    }

    vm_free(&ctx);
    kprintf("vm16_decimal_fault_test: PASSED\n");
    return 0;
}

/* T_DEC_SET / T_DEC_GET round trip through the VM. */
int vm16_decimal_setget_test(void)
{
    struct vm_ctx ctx;
    if (vm_init(&ctx) != 0) {
        kprintf("vm_setget_test FAIL: init\n");
        return 1;
    }
    const struct env_ops *env = bus_env();
    if (env == NULL) {
        kprintf("vm_setget_test FAIL: no env\n");
        vm_free(&ctx);
        return 1;
    }

    load_vm_str(&ctx, 0x3500, "test");
    load_vm_str(&ctx, 0x3510, "grav");
    /* value: 9876 at 0x3600, LE across 4 words (9876 = 0x2694) */
    ctx.mem[0x3600] = 0x2694;
    ctx.mem[0x3601] = 0x0000;
    ctx.mem[0x3602] = 0x0000;
    ctx.mem[0x3603] = 0x0000;

    /* SET: ns, name, value ptr, scale 3 / round HALF_EVEN(2) -> R4 = 0x0203 */
    ctx.reg[R0] = 0x3500;
    ctx.reg[R1] = 0x3510;
    ctx.reg[R2] = 0x3600;
    ctx.reg[R4] = 0x0203;

    uint16_t program[] = { 0xF0D1, 0xF0D0, 0xF025 };   /* SET, GET, HALT */
    if (vm_load_native(&ctx, VM_PC_START, program, 3) != 0) {
        kprintf("vm_setget_test FAIL: load\n");
        vm_free(&ctx);
        return 1;
    }
    if (vm_run(&ctx, 100) != VM_HALT) {
        kprintf("vm_setget_test FAIL: run\n");
        vm_free(&ctx);
        return 1;
    }

    /* GET wrote back to the same buffer at 0x3600; metadata in R3 */
    int64_t mag = (int64_t)(  (uint64_t)ctx.mem[0x3600]
                            | ((uint64_t)ctx.mem[0x3601] << 16)
                            | ((uint64_t)ctx.mem[0x3602] << 32)
                            | ((uint64_t)ctx.mem[0x3603] << 48));
    uint16_t meta = ctx.reg[R3];
    if (mag != 9876 || ctx.reg[R2] != 0x3600
        || (meta >> 8) != 3 || ((meta >> 4) & 0xF) != DEC_RND_HALF_EVEN
        || (meta & 0xF) != DEC_F_SIGNED) {
        kprintf("vm_setget_test FAIL: mag=%lld meta=%04X\n", (long long)mag, (unsigned)meta);
        vm_free(&ctx);
        return 1;
    }

    /* and it really landed in env, scale 3 */
    struct dec d = dec_get(env, "test", "grav");
    if (!d.ok || d.mag != 9876 || d.scale != 3) {
        kprintf("vm_setget_test FAIL: env mag=%lld scale=%u\n",
                (long long)d.mag, (unsigned)d.scale);
        vm_free(&ctx);
        return 1;
    }

    vm_free(&ctx);
    kprintf("vm16_decimal_setget_test: PASSED\n");
    return 0;
}

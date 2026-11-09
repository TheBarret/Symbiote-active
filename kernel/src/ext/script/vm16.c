#include <stddef.h>
#include <stdint.h>

#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/version.h>
#include <core/ext.h>
#include <core/cmd.h>
#include <core/bus.h>
#include <core/klog.h>
#include <core/kprintf.h>

#include <ext/script/decimal.h>
#include <ext/script/vm16.h>
#include <ext/script/test.h>

/* LC-3 references disagree on whether LEA sets the condition codes.
 * 0 = LEA leaves FLAGS untouched (original behaviour of this file),
 * 1 = LEA updates FLAGS like LD. Flip here if your toolchain expects it. */
#ifndef VM_LEA_SETS_CC
#define VM_LEA_SETS_CC 0
#endif

/*   decode   */

enum opcode { OP_BR = 0, OP_ADD, OP_LD, OP_ST, OP_JSR, OP_AND, OP_LDR,
              OP_STR, OP_RTI, OP_NOT, OP_LDI, OP_STI, OP_JMP, OP_RES,
              OP_LEA, OP_TRAP };

#define FL_POS 0x1   /* P - must match the LC-3 branch-condition encoding
                      * in bits [11:9]; see COND(i). Do not reorder. */
#define FL_ZRO 0x2   /* Z */
#define FL_NEG 0x4   /* N */

#define OPC(i)    (((uint16_t)(i) >> 12) & 0xF)
#define DR(i)     (((uint16_t)(i) >>  9) & 0x7)
#define SR1(i)    (((uint16_t)(i) >>  6) & 0x7)
#define SR2(i)    ((uint16_t)(i) & 0x7)
#define IMM5(i)   ((uint16_t)(i) & 0x1F)
#define MODE(i)   (((uint16_t)(i) >>  5) & 0x1)
#define PCOFF9(i) ((uint16_t)(i) & 0x1FF)
#define OFF6(i)   ((uint16_t)(i) & 0x3F)
#define TRAPV(i)  ((uint8_t)((i) & 0xFF))
#define LONGP(i)  (((uint16_t)(i) >> 11) & 0x1)
#define COND(i)   (((uint16_t)(i) >>  9) & 0x7)

/*   helpers   */

static inline uint16_t sext(uint16_t x, unsigned bits)
{
    if (x & (1u << (bits - 1)))
        x |= (uint16_t)(~0u << bits);
    return x;
}


_Static_assert(VM_MEMSIZE == 0x10000u, "vm16: vrd/vwr assume a full 16-bit address space");

static inline uint16_t vrd(const struct vm_ctx *c, uint16_t a)
{
    return c->mem[a];
}

static inline void vwr(struct vm_ctx *c, uint16_t a, uint16_t v)
{
    c->mem[a] = v;
}

static inline void update_flags(struct vm_ctx *c, uint16_t r)
{
    if (c->reg[r] == 0)         c->reg[FLAGS] = FL_ZRO;
    else if (c->reg[r] >> 15) c->reg[FLAGS] = FL_NEG;
    else                        c->reg[FLAGS] = FL_POS;
}

static inline void vm_raise(struct vm_ctx *c, enum vm_fault f)
{
    c->status = VM_FAULT;
    c->fault  = f;
}

/* Helper to read null-terminated ASCII strings packed into VM uint16_t words */
static void read_vm_str(const struct vm_ctx *c, uint16_t addr, char *dest, size_t max_len)
{
    size_t n = 0;
    for (size_t k = 0; (uint32_t)(addr + k) < VM_MEMSIZE && n < max_len - 1; k++) {
        uint16_t w = vrd(c, addr + (uint16_t)k);
        char c1 = (char)(w & 0xFF);
        if (c1 == '\0') { dest[n++] = '\0'; break; }
        dest[n++] = c1;
        if (n >= max_len - 1) { dest[n] = '\0'; break; }
        char c2 = (char)(w >> 8);
        if (c2 == '\0') { dest[n++] = '\0'; break; }
        dest[n++] = c2;
    }
    dest[max_len - 1] = '\0';
}

/*   traps   */

enum standard_trap { T_GETC = 0x20, T_OUT, T_PUTS, T_IN, T_PUTSP, T_HALT };

enum dec_trap {
    T_DEC_GET  = 0xD0,
    T_DEC_SET  = 0xD1,
    T_DEC_ADD  = 0xD2,
    T_DEC_SUB  = 0xD3,
    T_DEC_MUL  = 0xD4,
    T_DEC_DIV  = 0xD5,
    T_DEC_MOD  = 0xD6,
    T_DEC_CMP  = 0xD7,
};

static void trap(struct vm_ctx *c, uint16_t i)
{
    uint8_t vector = TRAPV(i);

    switch (vector) {
    case T_HALT:
        c->status = VM_HALT;
        return;

    case T_DEC_GET:
    case T_DEC_SET:
    case T_DEC_ADD:
    case T_DEC_SUB:
    case T_DEC_MUL:
    case T_DEC_DIV:
    case T_DEC_MOD:
    case T_DEC_CMP: {
        /* Register conventions:
         *   R0 = namespace string pointer (0 = kernel default namespace)
         *   R1 = operand A / source name string pointer
         *   R2 = operand B name (arith/cmp) | value ptr (GET dest, SET src)
         *   R3 = result name (arith) | packed metadata out (GET)
         *   R4 = scale (low byte; 0xFF = service default) << 8 | rounding (high nibble)
         *
         * Fault contract: any failure raises ctx->fault and stops the VM.
         * Condition codes are set ONLY by a successful T_DEC_CMP. R0 is
         * clobbered by CMP (result) and by arith traps (success indicator).
         * Stored importance is always 0: scripted data is evictable, the
         * self-regulating env reclaims it under pressure.
         */
        char ns_buf[32] = {0};
        char a_buf[32]  = {0};
        char b_buf[32]  = {0};
        char r_buf[32]  = {0};

        const char *ns = NULL;
        if (c->reg[R0] != 0) {
            read_vm_str(c, c->reg[R0], ns_buf, sizeof ns_buf);
            ns = ns_buf[0] ? ns_buf : NULL;
        }
        read_vm_str(c, c->reg[R1], a_buf, sizeof a_buf);

        const struct env_ops *env = bus_env();
        if (env == NULL) {
            vm_raise(c, VM_ERR_BADSTATE);
            return;
        }

        uint8_t scale_raw = (uint8_t)(c->reg[R4] & 0xFF);
        enum dec_round rnd = (enum dec_round)((c->reg[R4] >> 8) & 0x0F);

        /* Parameter validation happens HERE, before any decimal call:
         * a raw scale of 200 cast to int8_t would alias to -56 and silently
         * select the default scale; an invalid rounding nibble would silently
         * degrade to TRUNC inside dec_set. Both must fault instead.
         * 0xFF is the explicit "use the service default" selector. */
        bool scale_ok = (scale_raw == 0xFF) || (scale_raw <= DEC_MAX_SCALE);
        if (!scale_ok || !DEC_RND_VALID(rnd)) {
            vm_raise(c, VM_ERR_DEC_OP);
            return;
        }

        /* T_DEC_GET: fetch a decimal, serialize it back to the script.
         * value -> [R2 .. R2+3] little-endian; metadata word -> R3, laid out
         * exactly like DEC_P_ (scale<<8 | round<<4 | flags). */
        if (vector == T_DEC_GET) {
            struct dec d = dec_get(env, ns, a_buf);
            if (!d.ok) {
                vm_raise(c, VM_ERR_DEC_GET);
                return;
            }
            vm_write_i64(c, c->reg[R2], d.mag);
            c->reg[R3] = (uint16_t)(((uint16_t)d.scale << 8)
                                   | ((uint16_t)d.round << 4)
                                   | (uint16_t)d.flags);
            return;
        }

        /* T_DEC_SET: store a script-supplied value under scale/rounding.
         * Flags are forced to DEC_F_SIGNED: scripts do not get to choose
         * policy bits. Importance is fixed at 0 (see convention block). */
        if (vector == T_DEC_SET) {
            struct dec_signature sig = {
                .default_scale = scale_raw,
                .default_round = (uint8_t)rnd,
                .default_flags = DEC_F_SIGNED,
            };
            int64_t mag = vm_read_i64(c, c->reg[R2]);
            if (!dec_set(env, ns, a_buf, mag, (int8_t)scale_raw, &sig, 0)) {
                vm_raise(c, VM_ERR_DEC_STORE);
                return;
            }
            return;
        }

        /* Arithmetic / comparison: fetch both operands explicitly and fault
         * on fetch failure BEFORE use, instead of relying on the arithmetic
         * layer to re-validate a zeroed struct dec. */
        read_vm_str(c, c->reg[R2], b_buf, sizeof b_buf);
        struct dec op_a = dec_get(env, ns, a_buf);
        struct dec op_b = dec_get(env, ns, b_buf);
        if (!op_a.ok || !op_b.ok) {
            vm_raise(c, VM_ERR_DEC_GET);
            return;
        }

        if (vector == T_DEC_CMP) {
            int cmp_res = dec_cmp(op_a, op_b);
            if (cmp_res == -2) {
                /* Unreachable after the ok check above; kept as armor so a
                 * future dec_cmp change cannot leak a fault onto the CC. */
                vm_raise(c, VM_ERR_DEC_OP);
                return;
            }
            c->reg[R0] = (uint16_t)(cmp_res < 0 ? 0xFFFFu : (uint16_t)cmp_res);
            update_flags(c, R0);
            return;
        }

        uint8_t result_scale = scale_raw;
        struct dec res = {0};
        switch (vector) {
        case T_DEC_ADD: res = dec_add(op_a, op_b, rnd, result_scale); break;
        case T_DEC_SUB: res = dec_sub(op_a, op_b, rnd, result_scale); break;
        case T_DEC_MUL: res = dec_mul(op_a, op_b, rnd, result_scale); break;
        case T_DEC_DIV: res = dec_div(op_a, op_b, rnd, result_scale); break;
        case T_DEC_MOD: res = dec_mod(op_a, op_b, rnd, result_scale); break;
        default: break;
        }

        if (!res.ok) {
            vm_raise(c, VM_ERR_DEC_OP);
            return;
        }

        read_vm_str(c, c->reg[R3], r_buf, sizeof r_buf);
        if (!dec_set(env, ns, r_buf, res.mag, (int8_t)res.scale, NULL, 0)) {
            vm_raise(c, VM_ERR_DEC_STORE);
            return;
        }

        c->reg[R0] = 0; /* success indicator */
        return;
    }

    default:
        vm_raise(c, VM_ERR_UNIMPL);
        return;
    }
}

/*   opcode bodies   */

static void br(struct vm_ctx *c, uint16_t i)
{
    if (COND(i) & c->reg[FLAGS])
        c->reg[PC] = (uint16_t)(c->reg[PC] + sext(PCOFF9(i), 9));
}

static void add(struct vm_ctx *c, uint16_t i)
{
    uint16_t r = DR(i);
    c->reg[r] = (uint16_t)(c->reg[SR1(i)]
              + (MODE(i) ? sext(IMM5(i), 5) : c->reg[SR2(i)]));
    update_flags(c, r);
}

static void ld(struct vm_ctx *c, uint16_t i)
{
    uint16_t r = DR(i);
    c->reg[r] = vrd(c, (uint16_t)(c->reg[PC] + sext(PCOFF9(i), 9)));
    update_flags(c, r);
}

static void st(struct vm_ctx *c, uint16_t i)
{
    vwr(c, (uint16_t)(c->reg[PC] + sext(PCOFF9(i), 9)), c->reg[DR(i)]);
}

static void jsr(struct vm_ctx *c, uint16_t i)
{
    uint16_t target = LONGP(i)
        ? (uint16_t)(c->reg[PC] + sext((uint16_t)(i & 0x7FF), 11))
        : c->reg[SR1(i)];
    c->reg[R7] = c->reg[PC];
    c->reg[PC] = target;
}

static void and_(struct vm_ctx *c, uint16_t i)
{
    uint16_t r = DR(i);
    c->reg[r] = (uint16_t)(c->reg[SR1(i)]
              & (MODE(i) ? sext(IMM5(i), 5) : c->reg[SR2(i)]));
    update_flags(c, r);
}

static void ldr(struct vm_ctx *c, uint16_t i)
{
    uint16_t r = DR(i);
    c->reg[r] = vrd(c, (uint16_t)(c->reg[SR1(i)] + sext(OFF6(i), 6)));
    update_flags(c, r);
}

static void str(struct vm_ctx *c, uint16_t i)
{
    vwr(c, (uint16_t)(c->reg[SR1(i)] + sext(OFF6(i), 6)), c->reg[DR(i)]);
}

static void rti(struct vm_ctx *c, uint16_t i)
{
    (void)i;
    vm_raise(c, VM_ERR_ILLEGAL);
}

static void not_(struct vm_ctx *c, uint16_t i)
{
    uint16_t r = DR(i);
    c->reg[r] = (uint16_t)~c->reg[SR1(i)];
    update_flags(c, r);
}

static void ldi(struct vm_ctx *c, uint16_t i)
{
    uint16_t ptr = vrd(c, (uint16_t)(c->reg[PC] + sext(PCOFF9(i), 9)));
    uint16_t r   = DR(i);
    c->reg[r] = vrd(c, ptr);
    update_flags(c, r);
}

static void sti(struct vm_ctx *c, uint16_t i)
{
    uint16_t ptr = vrd(c, (uint16_t)(c->reg[PC] + sext(PCOFF9(i), 9)));
    vwr(c, ptr, c->reg[DR(i)]);
}

static void jmp(struct vm_ctx *c, uint16_t i)
{
    c->reg[PC] = c->reg[SR1(i)];
}

static void res(struct vm_ctx *c, uint16_t i)
{
    (void)i;
    vm_raise(c, VM_ERR_ILLEGAL);
}

static void lea(struct vm_ctx *c, uint16_t i)
{
    uint16_t r = DR(i);
    c->reg[r] = (uint16_t)(c->reg[PC] + sext(PCOFF9(i), 9));
#if VM_LEA_SETS_CC
    update_flags(c, r);
#endif
}

/*   dispatch table   */

typedef void (*op_fn)(struct vm_ctx *, uint16_t);

static const op_fn op_ex[VM_NOPS] = {
    br, add, ld, st, jsr, and_, ldr, str,
    rti, not_, ldi, sti, jmp, res, lea, trap
};

/*   public API   */

int vm_init(struct vm_ctx *c)
{
    if (c == NULL) return -1;

    c->mem = kzalloc(VM_MEMSIZE * sizeof(uint16_t));
    if (c->mem == NULL) return -1;

    for (unsigned r = 0; r < RCOUNT; r++)
        c->reg[r] = 0;
    c->reg[PC]     = VM_PC_START;
    c->reg[FLAGS]  = FL_ZRO;

    c->fuel_remaining = 0;
    c->status   = VM_OK;
    c->fault    = VM_ERR_NONE;
    c->fault_pc = 0;
    return 0;
}

/* vm statics */

/* int64 in VM memory: little-endian across 4 words, so script data crosses
 * the boundary as a serialized payload, never as a raw kernel pointer. */
int64_t vm_read_i64(const struct vm_ctx *c, uint16_t a)
{
    uint64_t v = 0;
    for (int k = 3; k >= 0; k--)
        v = (v << 16) | vrd(c, (uint16_t)(a + (uint16_t)k));
    return (int64_t)v;
}

void vm_write_i64(struct vm_ctx *c, uint16_t a, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int k = 0; k < 4; k++) {
        vwr(c, (uint16_t)(a + (uint16_t)k), (uint16_t)(u & 0xFFFFu));
        u >>= 16;
    }
}

int vm_load_native(struct vm_ctx *c, uint32_t origin,
                   const uint16_t *image, size_t nwords)
{
    if (c == NULL || c->mem == NULL) return -1;
    if (nwords != 0 && image == NULL) return -1;
    if (origin > VM_MEMSIZE || nwords > VM_MEMSIZE - origin) return -1;
    for (size_t k = 0; k < nwords; k++)
        c->mem[origin + k] = image[k];
    return 0;
}

int vm_load(struct vm_ctx *c, uint32_t origin,
            const uint8_t *args, size_t args_len)
{
    if (c == NULL || c->mem == NULL) return -1;
    if (args_len != 0 && args == NULL) return -1;
    if (args_len & 1u)               return -1;
    size_t nwords = args_len / 2;
    if (origin > VM_MEMSIZE || nwords > VM_MEMSIZE - origin) return -1;
    for (size_t k = 0; k < nwords; k++) {
        uint16_t w = (uint16_t)args[2*k] | ((uint16_t)args[2*k + 1] << 8);
        c->mem[origin + k] = w;
    }
    return 0;
}

enum vm_status vm_step(struct vm_ctx *c)
{
    if (c == NULL) return VM_FAULT;
    if (c->mem == NULL) {
        vm_raise(c, VM_ERR_BADSTATE);
        c->fault_pc = c->reg[PC];
        return c->status;
    }

    if (c->status != VM_OK) return c->status;

    uint16_t pc0   = c->reg[PC];
    uint16_t instr = vrd(c, pc0);
    c->reg[PC]     = (uint16_t)(pc0 + 1u);

    uint16_t op = OPC(instr);
    if (op >= VM_NOPS) {
        vm_raise(c, VM_ERR_ILLEGAL);
    } else {
        op_ex[op](c, instr);
    }

    if (c->status == VM_FAULT)
        c->fault_pc = pc0;
    return c->status;
}

enum vm_status vm_run(struct vm_ctx *c, uint32_t fuel)
{
    if (c == NULL) return VM_FAULT;
    c->fuel_remaining = fuel;

    while (c->status == VM_OK) {
        if (c->fuel_remaining == 0) return VM_YIELD;
        c->fuel_remaining--;
        vm_step(c);
    }
    return c->status;
}

void vm_stop(struct vm_ctx *c)
{
    if (c) c->status = VM_HALT;
}

void vm_free(struct vm_ctx *c)
{
    if (c == NULL) return;
    if (c->mem) {
        kfree(c->mem);
        c->mem = NULL;
    }
    c->status = VM_HALT;
    c->fault  = VM_ERR_NONE;
}

static const struct vm_ops g_vm_ops = {
    .init = vm_init,
    .load = vm_load,
    .run  = vm_run,
    .stop = vm_stop,
    .free = vm_free,
};

int vm_ext_init(void)
{
    struct bus_device dev = {
        .name         = "vm16",
        .class        = BUS_CLASS_VM16,
        .version      = 1,
        .ops          = &g_vm_ops,
        .private_data = NULL,
        .owner_id     = BUS_VM16,
    };
    if (bus_register(&dev) != 0) { return -1; }
    bus_test("vm16");
    return 0;
}

static int cmd_vm16_fn(const struct cmd_args *a) {
    (void)a;
    kprintf("Running vm16 selftesters...\n");
    dec_selftest();                     // decimal.h assert test
    vm16_decimal_integration_test();    // vm16.c decimal.h integration test
    vm16_decimal_fault_test();       // cmp against missing symbol -> VM_ERR_DEC_GET
    vm16_decimal_setget_test();     // T_DEC_SET/T_DEC_GET round trip
    return CMD_OK;
}

SYM_COMMAND(vm16test, "", "Run vm16 testers", cmd_vm16_fn);

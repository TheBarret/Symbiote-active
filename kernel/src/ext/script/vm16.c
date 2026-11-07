#include <stddef.h>
#include <stdint.h>

#include <lib/mem.h>
#include <lib/string.h>
#include <core/arch/heap.h>
#include <core/version.h>
#include <core/bus.h>
#include <core/config.h>
#include <core/ext.h>
#include <core/klog.h>

#include <ext/script/decimal.h>
#include <ext/script/vm16.h>

/* LC-3 references disagree on whether LEA sets the condition codes.
 * 0 = LEA leaves FLAGS untouched (original behaviour of this file),
 * 1 = LEA updates FLAGS like LD. Flip here if your toolchain expects it. */
#ifndef VM_LEA_SETS_CC
#define VM_LEA_SETS_CC 0
#endif

/*  decode  */

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

/*  helpers  */

static inline uint16_t sext(uint16_t x, unsigned bits)
{
    if (x & (1u << (bits - 1)))
        x |= (uint16_t)(~0u << bits);
    return x;
}

/* VM memory is exactly 64K words, so a 16-bit address can never be out of range.
 * If VM_MEMSIZE ever shrinks, these two helpers are the only place that needs a bounds check. */
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
    if (c->reg[r] == 0)       c->reg[FLAGS] = FL_ZRO;
    else if (c->reg[r] >> 15) c->reg[FLAGS] = FL_NEG;
    else                      c->reg[FLAGS] = FL_POS;
}

static inline void vm_raise(struct vm_ctx *c, enum vm_fault f)
{
    c->status = VM_FAULT;
    c->fault  = f;
}

/*  traps (stage 1: stub)  */

enum trap { T_GETC = 0x20, T_OUT, T_PUTS, T_IN, T_PUTSP, T_HALT };

static void trap(struct vm_ctx *c, uint16_t i)
{
    switch (TRAPV(i)) {
    case T_HALT:
        c->status = VM_HALT;
        return;
    default:
        /* Stage 2 will route these through bus_proxy(). not implemented. */
        vm_raise(c, VM_ERR_UNIMPL);
        return;
    }
}

/*  opcode bodies  */

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
    /* Resolve the target BEFORE clobbering R7: JSRR R7 must jump to the old R7, not to the return address. */
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
    /* No interrupts or privilege modes in stage 1. Fault loudly rather than silently NOP so broken programs are noticed. */
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

/*  dispatch table  */

typedef void (*op_fn)(struct vm_ctx *, uint16_t);

static const op_fn op_ex[VM_NOPS] = {
    br, add, ld, st, jsr, and_, ldr, str,
    rti, not_, ldi, sti, jmp, res, lea, trap
};

/*  public API  */

int vm_init(struct vm_ctx *c)
{
    if (c == NULL) return -1;

    /* The kernel heap is a boot invariant by the time extensions load.
     * Do NOT call heap_init() here.
     *
     * kzalloc, not kmalloc: scripts must never observe stale kernel heap contents (pointers, env values),
     * and zeroed memory makes program behaviour deterministic in both fast and SYM_MEMDEBUG builds. */
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

int vm_load_native(struct vm_ctx *c, uint32_t origin,
                   const uint16_t *image, size_t nwords)
{
    if (c == NULL || c->mem == NULL) return -1;
    if (nwords != 0 && image == NULL) return -1;
    /* Written to be wrap-proof: no addition on attacker-sized values. */
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
    if (args_len & 1u)                return -1;
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
        /* Latch the fault in the ctx so vm_run() terminates instead of spinning until fuel runs out. */
        vm_raise(c, VM_ERR_BADSTATE);
        c->fault_pc = c->reg[PC];
        return c->status;
    }

    /* A halted/faulted ctx must not execute another instruction. */
    if (c->status != VM_OK) return c->status;

    uint16_t pc0   = c->reg[PC];
    uint16_t instr = vrd(c, pc0);
    c->reg[PC]     = (uint16_t)(pc0 + 1u);       /* fetch + auto-advance */

    uint16_t op = OPC(instr);
    if (op >= VM_NOPS) {                          /* unreachable with a 4-bit */
        vm_raise(c, VM_ERR_ILLEGAL);              /* field; kept as a guard   */
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
        /* SYM_MEMDEBUG poisons freed blocks; a stale c->mem read trips 0xDD. */
        kfree(c->mem);
        c->mem = NULL;
    }
    /* HALT, not OK: a freed ctx must not look runnable. vm_init() resets it. */
    c->status = VM_HALT;
    c->fault  = VM_ERR_NONE;
}

/*  bus extension  */

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
    if (bus_register(&dev) != 0)
        return -1;
    bus_test("vm16");
    return 0;
}

//SYM_EXTENSION(script, vm_ext_init, EXT_PRIO_SERVICE);

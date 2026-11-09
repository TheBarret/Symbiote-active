#ifndef EXT_SCRIPT_VM16_H
#define EXT_SCRIPT_VM16_H

#include <stddef.h>
#include <stdint.h>

/* vm16, LC-3-style 16-bit VM core for Symbiote.
 *
 * Stage 1 scope:
 *   - pure compute machine, no external I/O
 *   - memory from kernel heap, ZEROED (kzalloc): program behavior is
 *     deterministic and no stale kernel heap bytes are visible to scripts
 *   - load via serialized u16-LE byte payload (proxy-shaped args)
 *   - bounded execution via fuel counter
 *   - trap() is a stub: T_HALT -> VM_HALT, everything else -> VM_FAULT/UNIMPL
 *   - registers as bus device "vm16" with owner_id BUS_VM16
 *
 * Context lifetime contract:
 *   vm_init() expects a ctx that is either zero-initialised or has been
 *   through vm_free(). Calling it on a live ctx leaks ctx->mem.
 *
 * Stage 2 will add: bus_proxy() dispatch inside trap(), wall-clock deadline,
 * T_PROXY_* encoding. None of that appears here.
 */

#define VM_MEMSIZE   0x10000u   /* words (128 KiB of uint16_t) */
#define VM_PC_START  0x3000u
#define VM_NOPS      16u

enum vm_status {
    VM_OK = 0,      /* ready / running */
    VM_HALT,        /* program halted cleanly, or ctx freed/stopped */
    VM_YIELD,       /* fuel exhausted, resumable (returned by vm_run only) */
    VM_FAULT,       /* illegal opcode, unimplemented trap, bad state */
};

enum vm_fault {
    VM_ERR_NONE = 0,
    VM_ERR_ILLEGAL,     /* reserved opcode, or RTI (no interrupts yet) */
    VM_ERR_UNIMPL,      /* trap vector not implemented in stage 1 */
    VM_ERR_BADPC,       /* reserved: PC range check (cannot fire at 64K) */
    VM_ERR_BADSTATE,    /* NULL memory / uninitialised ctx / env device absent */
    VM_ERR_DEC_GET,     /* dec trap: operand fetch failed (missing, mistyped, corrupt mask) */
    VM_ERR_DEC_OP,      /* dec trap: bad scale/rounding parameter, or failed arithmetic */
    VM_ERR_DEC_STORE,   /* dec trap: result could not be stored back into env */
};
/* Registers, state flags */
enum regist { R0 = 0, R1, R2, R3, R4, R5, R6, R7, PC, FLAGS, RCOUNT };

struct vm_ctx {
    uint16_t        *mem;               /* VM_MEMSIZE words, kzalloc'd */
    uint16_t         reg[RCOUNT];
    uint32_t         fuel_remaining;
    enum vm_status   status;
    enum vm_fault    fault;
    uint16_t         fault_pc;          /* address of the faulting instruction;
                                         * only meaningful when status == VM_FAULT */
};

struct vm_ops {
    int             (*init)  (struct vm_ctx *c);
    int             (*load)  (struct vm_ctx *c, uint32_t origin,
                              const uint8_t *args, size_t args_len);
    enum vm_status  (*run)   (struct vm_ctx *c, uint32_t fuel);
    void            (*stop)  (struct vm_ctx *c);
    void            (*free)  (struct vm_ctx *c);
};

/* helpers */
int64_t vm_read_i64(const struct vm_ctx *c, uint16_t a);
void vm_write_i64(struct vm_ctx *c, uint16_t a, int64_t v);

/* Direct API, for in-tree tests; identical semantics to g_vm_ops members. */
int             vm_init(struct vm_ctx *c);
int             vm_load(struct vm_ctx *c, uint32_t origin,
                        const uint8_t *args, size_t args_len);
int             vm_load_native(struct vm_ctx *c, uint32_t origin,
                               const uint16_t *image, size_t nwords);
enum vm_status  vm_step(struct vm_ctx *c);
enum vm_status  vm_run (struct vm_ctx *c, uint32_t fuel);
void            vm_stop(struct vm_ctx *c);
void            vm_free(struct vm_ctx *c);

/* expose init */
int vm_ext_init(void);

#endif

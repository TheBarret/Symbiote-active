#ifndef CORE_SERVICES_VM_H
#define CORE_SERVICES_VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* codebase: 0.2.0 */

/* 16bit Virtual Machine
 *
 * Frame Class  | Port | Purpose
 * -------------|------|-----------------------
 * Signal       |    0 | broadcast, never use!
 * Signal       |    1 | default service (foreign control)
 * Signal       |  1+n | selector bank, ROM frames

 *
 * ROM payload layout (signal frame on a bank port):
 *   byte 0..1   magic word 1
 *   byte 2..3   magic word 2
 *   byte 4..5   header (uint16, reset policy / load mode / signature)
 *   byte 6..n   opcode stream
 *
 * Bank memory is a range inside the shared vm_state.memory.
 * Banks hold CPU state (registers, PC, SP, buffer), not the memory.
 *
 * changelog 0.1 (baseline):
 * - Pipeline: `fetch` → decode → execute → `PC` advance. `vm_step` is one instruction, `vm_run` is a budgeted loop.
 *   The fetch bounds-check against `[base, end)` is the range guard, a ROM can't execute outside its loaded image.
 * - Tier 1 ALU: `NOP`, `MOV`, `LDI`, `ADD`, `SUB`, `AND`, `OR`, `XOR`, `SHL`, `SHR`, `CMP`, `HALT`.
 *   Register-register ops take a source register index; `LDI`/`SHL`/`SHR` take an immediate byte.
 *   `CMP` sets `VM_FLAG_Z` for tier 2 branching (when it lands).
 * - Faults: `VM_FAULT_PC` (fetch out of range), `VM_FAULT_OPCODE` (unknown op, register index > 15).
 *   Budget exhaustion sets `VM_FAULT_BUDGET` after the loop.
 *   `HALT` sets `running = false` and `last_fault = VM_OK`, the clean-exit signal.
 * - `vm_bank_load` splices at `bank->base`, sets `end`, resets `PC`/`SP`/`I`/`flags`, marks `loaded`/`running`.
 *   Bounds-checked against `bank->end - bank->base`. The caller is responsible for setting `base` before calling.
 * - Absolute 16-bit addresses, big-endian in the stream, assembler design primed to emit absolute targets;
 *   no relative offsets, no PC-relative math, no base-address bookkeeping in the ROM.
 *   target_ok is the single bounds check: a jump must land inside (base, end).
 *   That's the range guard for branching, mirroring fetch's guard for linear execution.
 * - JMP/JZ/JNZ set PC directly and return true, bypassing the pc_advance path.
 *   JZ/JNZ only take that path when the branch is not taken, in which case pc_advance = 3 runs normally.
 *   That keeps the taken/not-taken logic local and makes the "PC set directly, no advance" invariant explicit at each site.
 * - CALL pushes PC + 3, the return address of the next instruction after the 3-byte CALL.
 *   RET pops into PC directly. SP is uint16_t in vm_state (header already has it that way),
 *   so SP < 16 and SP > 0 are the bounds. VM_FAULT_STACK covers both call overflow and return underflow.
 * - buffer[16] is fixed, so the subroutine depth is 16. CALL at depth 16 faults. That's the stack limit,
 *   and it's enforced in one place.
 */

/* (ROM) image magic, two 16-bit words marker for detection only */
#define VM_M0      0x564Du     /* "VM" */
#define VM_M1      0x3136u     /* "16" */

/* port policy */
#define VM_PORT             1u          /* port 1, wake-up */
#define VM_BANK_BASE        2u          /* port 2, selector, ROM */
#define VM_BANK_MAX         8u          /* port banks 0..7 -> ports 2..9 */

/* per-step budget cap */
#define VM_BUDGET_MAX   4096u

/* memory layout: one 64 KiB address space, banks splice into disjoint ranges */
#define VM_MEM_SIZE     65536u

/* Flags */
#define VM_FLAG_Z  (1u << 0)

/* syscall ceiling */
#define VM_SYSCALL_MAX  16

enum vm_fault {
    VM_OK = 0,
    VM_FAULT_OPCODE,        /* invalid opcode byte */
    VM_FAULT_PC,            /* PC outside the loaded image */
    VM_FAULT_STACK,         /* SP overflow on CALL */
    VM_FAULT_BUDGET,        /* step budget exhausted */
    VM_FAULT_FEATURE,       /* SYS id outside the bank's class */
    VM_FAULT_RANGE,         /* load/splice outside the bank's range */
    VM_FAULT_SYSCALL        /* Invalid system call instruction */
};

/* OPCODES Tiers
 * Encoding: [op][a][b]
 *   op   = opcode byte
 *   a    = destination register index (0..15)
 *   b    = source register index OR immediate, per opcode
 */
enum vm_op {
    /* Tier 1 (ALU) */
    VM_OP_NOP = 0x00,   /* [op]                       1 byte  */
    VM_OP_MOV = 0x01,   /* [op][dst][src_reg]         3 bytes */
    VM_OP_LDI = 0x02,   /* [op][dst][imm8]            3 bytes */
    VM_OP_ADD = 0x03,   /* [op][dst][src_reg]         3 bytes */
    VM_OP_SUB = 0x04,   /* [op][dst][src_reg]         3 bytes */
    VM_OP_AND = 0x05,   /* [op][dst][src_reg]         3 bytes */
    VM_OP_OR  = 0x06,   /* [op][dst][src_reg]         3 bytes */
    VM_OP_XOR = 0x07,   /* [op][dst][src_reg]         3 bytes */
    VM_OP_SHL = 0x08,   /* [op][dst][imm8]            3 bytes */
    VM_OP_SHR = 0x09,   /* [op][dst][imm8]            3 bytes */
    VM_OP_CMP = 0x0A,   /* [op][dst][src_reg]         3 bytes, sets Z flag */
    /* 0x0B...0x0E */
    VM_OP_HALT = 0x0F,  /* [op]                        stop program */

    /* Tier 2 Branching */
    VM_OP_JMP  = 0x10,   /* [op][hi][lo]         unconditional */
    VM_OP_JZ   = 0x11,   /* [op][hi][lo]         jump if Z set */
    VM_OP_JNZ  = 0x12,   /* [op][hi][lo]         jump if Z clear */
    VM_OP_CALL = 0x13,   /* [op][hi][lo]         push PC+3, jump */
    VM_OP_RET  = 0x14,   /* [op]                 pop PC */
    /* 0x15...0x1F */

    /* Tier 3 System calls */
    VM_OP_SYS = 0x20, /* [op][hi][lo] 16bit system call id */
};

typedef struct {
    uint16_t    R[16];          /* general purpose registers */
    uint16_t    I;              /* index register */
    uint16_t    PC;             /* program counter */
    uint16_t    SP;             /* stack pointer (index into buffer) */
    uint16_t    FLAGS;          /* general purpose flags */
    uint16_t    buffer[16];     /* subroutine stack */
    enum vm_fault last_fault;   /* error metric, set by vm_step on failure */
} vm_state;

struct vm_bank {
    uint8_t      class;         /* system call namespace this bank may SYS into */
    uint32_t     base;          /* where its ROM image lands in memory */
    uint32_t     end;           /* end of loaded image */
    vm_state     state;         /* CPU state for this bank */
    bool         loaded;        /* image present */
    bool         running;       /* not halted / faulted */
    uint32_t     budget;        /* steps remaining this frame */
    uint32_t     budget_max;    /* reload value on each frame */
};

/* shared memory */
extern uint8_t vm_memory[VM_MEM_SIZE];

struct vm_bank;

/* bank lifecycle: caller sets base before load; capacity is VM_MEM_SIZE - base */
void   vm_bank_init(struct vm_bank *bank, uint8_t class, uint32_t budget_max);
bool   vm_bank_load(struct vm_bank *bank, const uint8_t *code, size_t len);

/* execution */
bool   vm_step(struct vm_bank *bank);
size_t vm_run (struct vm_bank *bank, uint32_t budget_max);

/* syscall table */
typedef bool (*vm_syscall_fn)(struct vm_bank *bank);
bool vm_syscall_register(uint8_t class, uint16_t id, vm_syscall_fn fn, const char *name);

/* one-shot: validate a signal-frame payload, splice into the foreground bank,
 * run to halt/fault/budget. Returns the final fault (VM_OK on clean halt). */
enum vm_fault vm_service_run(const uint8_t *payload, size_t len);

/* System functions
 * one entry per (class, id)
 */
struct vm_syscall_slot {
    uint8_t       class;
    uint16_t         id;
    vm_syscall_fn    fn;
    const char    *name;
};

#endif

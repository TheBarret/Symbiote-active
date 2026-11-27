#ifndef CORE_SERVICES_VM_H
#define CORE_SERVICES_VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <core/services/router.h>

/* codebase: 0.2.0 */

/* VM16: one-shot 16-bit micro machine. Serves SIGNAL frames on VM_PORT.
 *
 * Payload : "VM16" (4 bytes) | header (2 bytes, must be 0) | code (1..VM_CODE_MAX bytes)
 *
 * The code runs in place inside the frame, which outlives the run. Code
 * addresses start at VM_ORG; jump, call and SYS operands are absolute and
 * big-endian. There is no data memory: state is R0..R15, I (reserved), PC, SP,
 * FLAGS and a 16-deep return stack.
 */

#define VM_PORT         1u              /* the only port the VM serves */
#define VM_ENVELOPE     6u              /* magic(4) + header(2) */
#define VM_CODE_MAX     (ROUTER_MAX_PAYLOAD - VM_ENVELOPE)
#define VM_ORG          0x0200u         /* logical address of the first code byte */
#define VM_BUDGET_MAX   4096u           /* instruction budget per run */
#define VM_SYSCALL_MAX  16

_Static_assert(VM_ORG + VM_CODE_MAX <= 0x10000u, "vm.h: code image must fit the 16-bit address space");

#define VM_FLAG_Z  (1u << 0)

enum vm_fault {
    VM_OK = 0,
    VM_FAULT_OPCODE,        /* unknown opcode, or register index > 15 */
    VM_FAULT_PC,            /* fetch or branch target outside the image */
    VM_FAULT_STACK,         /* CALL overflow or RET underflow */
    VM_FAULT_BUDGET,        /* step budget exhausted */
    VM_FAULT_ENVELOPE,      /* bad size, magic or header */
    VM_FAULT_BUSY,          /* a run is already in progress */
    VM_FAULT_SYSCALL        /* unknown SYS id for this class, or the handler failed */
};

/* Encoding: [op][a][b]
 *   a = destination register index (0..15)
 *   b = source register index or immediate, per opcode
 */
enum vm_op {
    VM_OP_NOP  = 0x00,  /* [op]                      */
    VM_OP_MOV  = 0x01,  /* [op][dst][src]            */
    VM_OP_LDI  = 0x02,  /* [op][dst][imm8]           */
    VM_OP_ADD  = 0x03,  /* [op][dst][src]            */
    VM_OP_SUB  = 0x04,  /* [op][dst][src]            */
    VM_OP_AND  = 0x05,  /* [op][dst][src]            */
    VM_OP_OR   = 0x06,  /* [op][dst][src]            */
    VM_OP_XOR  = 0x07,  /* [op][dst][src]            */
    VM_OP_SHL  = 0x08,  /* [op][dst][imm8]           */
    VM_OP_SHR  = 0x09,  /* [op][dst][imm8]           */
    VM_OP_CMP  = 0x0A,  /* [op][dst][src]  sets Z    */
    VM_OP_HALT = 0x0F,  /* [op]            clean stop */

    VM_OP_JMP  = 0x10,  /* [op][hi][lo]              */
    VM_OP_JZ   = 0x11,  /* [op][hi][lo]    if Z set   */
    VM_OP_JNZ  = 0x12,  /* [op][hi][lo]    if Z clear */
    VM_OP_CALL = 0x13,  /* [op][hi][lo]    push PC+3  */
    VM_OP_RET  = 0x14,  /* [op]            pop PC     */

    VM_OP_SYS  = 0x20,  /* [op][hi][lo]    16-bit syscall id */
};

/* Syscall handler. It sees only the register file: arguments in R0..R3,
 * result in R0. Return false to fault the run.
 */
typedef bool (*vm_syscall_fn)(uint16_t *R);

/* One entry per (class, id). The class is chosen by the kernel and passed to
 * vm_service_run(), never read from the ROM. Register during extension init;
 * refused while a run is active.
 */
bool vm_syscall_register(uint8_t class, uint16_t id, vm_syscall_fn fn, const char *name);

/* Validate a signal payload, run it in place, return the final fault
 * (VM_OK on clean HALT). One run at a time.
 */
enum vm_fault vm_service_run(uint8_t class, const uint8_t *payload, size_t len);

const char *vm_fault_name(enum vm_fault f);

#endif /* CORE_SERVICES_VM_H */

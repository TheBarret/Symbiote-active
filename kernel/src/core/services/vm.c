#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/klog.h>
#include <core/services/vm.h>
#include <core/services/logger.h>

uint8_t vm_memory[VM_MEM_SIZE];

/* single rom operation */
static struct vm_bank g_bank;
static bool           g_bank_in_use;

/* syscall table, frozen after init */
static struct vm_syscall_slot vm_syscall[VM_SYSCALL_MAX];
static uint32_t               vm_syscall_count;

/*  bank */

void vm_bank_init(struct vm_bank *bank, uint8_t class, uint32_t budget_max) {
    if (!bank) return;
    memset(bank, 0, sizeof *bank);
    bank->class      = class;
    bank->budget_max = budget_max;
    bank->state.last_fault = VM_OK;
    log_write("       vm_bank_init(class=%u,budget=%u)\n",
              (unsigned)class, (unsigned)budget_max);
}

bool vm_bank_load(struct vm_bank *bank, const uint8_t *code, size_t len) {
    if (!bank || !code || len == 0)          return false;
    if (bank->base >= VM_MEM_SIZE)           return false;
    if (len > (size_t)(VM_MEM_SIZE - bank->base)) return false;

    memcpy(vm_memory + bank->base, code, len);

    bank->end              = bank->base + (uint32_t)len;
    bank->state.PC         = (uint16_t)bank->base;
    bank->state.SP         = 0;
    bank->state.I          = 0;
    bank->state.FLAGS      = 0;
    bank->state.last_fault = VM_OK;
    bank->loaded           = true;
    bank->running          = true;
    bank->budget           = bank->budget_max;

    log_write("       vm_bank_load(base=0x%04x,end=0x%04x)\n",
              (unsigned)bank->base, (unsigned)bank->end);
    return true;
}

/* fetch */

static bool fetch(struct vm_bank *bank, uint16_t off, uint8_t *out) {
    uint32_t addr = (uint32_t)bank->state.PC + off;
    if (addr < bank->base)     return false;
    if (addr >= bank->end)     return false;
    if (addr >= VM_MEM_SIZE)   return false;
    *out = vm_memory[addr];
    return true;
}

static bool fetch_addr(struct vm_bank *bank, uint16_t off, uint16_t *out) {
    uint8_t hi, lo;
    if (!fetch(bank, off, &hi))                 return false;
    if (!fetch(bank, (uint16_t)(off + 1), &lo)) return false;
    *out = (uint16_t)(((uint16_t)hi << 8) | lo);
    return true;
}

static bool target_ok(struct vm_bank *bank, uint16_t addr) {
    return addr >= bank->base && addr < bank->end;
}

/* syscalls */

bool vm_syscall_register(uint8_t class, uint16_t id, vm_syscall_fn fn, const char *name) {
    if (!fn || !name)                          return false;
    if (vm_syscall_count >= VM_SYSCALL_MAX)    return false;
    for (uint32_t i = 0; i < vm_syscall_count; i++) {
        if (vm_syscall[i].class == class && vm_syscall[i].id == id)
            return false;
    }
    vm_syscall[vm_syscall_count].class = class;
    vm_syscall[vm_syscall_count].id    = id;
    vm_syscall[vm_syscall_count].fn    = fn;
    vm_syscall[vm_syscall_count].name  = name;
    vm_syscall_count++;
    return true;
}

static vm_syscall_fn vm_syscall_lookup(uint8_t class, uint16_t id) {
    for (uint32_t i = 0; i < vm_syscall_count; i++) {
        if (vm_syscall[i].class == class && vm_syscall[i].id == id)
            return vm_syscall[i].fn;
    }
    return NULL;
}

/* step */

bool vm_step(struct vm_bank *bank) {
    if (!bank || !bank->running || !bank->loaded)
        return false;

    uint8_t op;
    if (!fetch(bank, 0, &op)) {
        bank->state.last_fault = VM_FAULT_PC;
        bank->running = false;
        return false;
    }

    uint16_t pc_advance = 1;

    switch (op) {

    /* tier 1 */
    case VM_OP_NOP:
        break;

    case VM_OP_MOV: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        bank->state.R[a] = bank->state.R[b];
        pc_advance = 3;
        break;
    }

    case VM_OP_LDI: {
        uint8_t a, imm;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &imm)) goto fault_pc;
        if (a > 15) goto fault_op;
        bank->state.R[a] = imm;
        pc_advance = 3;
        break;
    }

    case VM_OP_ADD: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        bank->state.R[a] = (uint16_t)(bank->state.R[a] + bank->state.R[b]);
        pc_advance = 3;
        break;
    }

    case VM_OP_SUB: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        bank->state.R[a] = (uint16_t)(bank->state.R[a] - bank->state.R[b]);
        pc_advance = 3;
        break;
    }

    case VM_OP_AND: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        bank->state.R[a] &= bank->state.R[b];
        pc_advance = 3;
        break;
    }

    case VM_OP_OR: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        bank->state.R[a] |= bank->state.R[b];
        pc_advance = 3;
        break;
    }

    case VM_OP_XOR: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        bank->state.R[a] ^= bank->state.R[b];
        pc_advance = 3;
        break;
    }

    case VM_OP_SHL: {
        uint8_t a, imm;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &imm)) goto fault_pc;
        if (a > 15) goto fault_op;
        bank->state.R[a] = (uint16_t)(bank->state.R[a] << (imm & 15));
        pc_advance = 3;
        break;
    }

    case VM_OP_SHR: {
        uint8_t a, imm;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &imm)) goto fault_pc;
        if (a > 15) goto fault_op;
        bank->state.R[a] = (uint16_t)(bank->state.R[a] >> (imm & 15));
        pc_advance = 3;
        break;
    }

    case VM_OP_CMP: {
        uint8_t a, b;
        if (!fetch(bank, 1, &a) || !fetch(bank, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        if (bank->state.R[a] == bank->state.R[b])
            bank->state.FLAGS |= VM_FLAG_Z;
        else
            bank->state.FLAGS &= (uint16_t)~VM_FLAG_Z;
        pc_advance = 3;
        break;
    }

    /* tier 2 */
    case VM_OP_JMP: {
        uint16_t tgt;
        if (!fetch_addr(bank, 1, &tgt)) goto fault_pc;
        if (!target_ok(bank, tgt))      goto fault_pc;
        bank->state.PC = tgt;
        return true;
    }

    case VM_OP_JZ: {
        uint16_t tgt;
        if (!fetch_addr(bank, 1, &tgt)) goto fault_pc;
        if (!target_ok(bank, tgt))      goto fault_pc;
        if (bank->state.FLAGS & VM_FLAG_Z) {
            bank->state.PC = tgt;
            return true;
        }
        pc_advance = 3;
        break;
    }

    case VM_OP_JNZ: {
        uint16_t tgt;
        if (!fetch_addr(bank, 1, &tgt)) goto fault_pc;
        if (!target_ok(bank, tgt))      goto fault_pc;
        if (!(bank->state.FLAGS & VM_FLAG_Z)) {
            bank->state.PC = tgt;
            return true;
        }
        pc_advance = 3;
        break;
    }

    case VM_OP_CALL: {
        uint16_t tgt;
        if (!fetch_addr(bank, 1, &tgt)) goto fault_pc;
        if (!target_ok(bank, tgt))      goto fault_pc;
        if (bank->state.SP >= 16)       goto fault_stack;
        bank->state.buffer[bank->state.SP++] = (uint16_t)(bank->state.PC + 3);
        bank->state.PC = tgt;
        return true;
    }

    case VM_OP_RET: {
        if (bank->state.SP == 0) goto fault_stack;
        bank->state.PC = bank->state.buffer[--bank->state.SP];
        return true;
    }

    /* tier 3 */
    case VM_OP_SYS: {
        uint16_t id;
        if (!fetch_addr(bank, 1, &id)) goto fault_pc;
        vm_syscall_fn fn = vm_syscall_lookup(bank->class, id);
        if (!fn)         goto fault_sys;
        if (!fn(bank))   goto fault_sys;
        pc_advance = 3;
        break;
    }

    case VM_OP_HALT:
        bank->state.PC = (uint16_t)(bank->state.PC + 1);
        bank->running  = false;
        bank->state.last_fault = VM_OK;
        return true;

    default:
        goto fault_op;
    }

    bank->state.PC = (uint16_t)(bank->state.PC + pc_advance);
    return true;

fault_sys:
    bank->state.last_fault = VM_FAULT_SYSCALL;
    bank->running = false;
    return false;

fault_stack:
    bank->state.last_fault = VM_FAULT_STACK;
    bank->running = false;
    return false;

fault_pc:
    bank->state.last_fault = VM_FAULT_PC;
    bank->running = false;
    return false;

fault_op:
    bank->state.last_fault = VM_FAULT_OPCODE;
    bank->running = false;
    return false;
}

/* run instance */

size_t vm_run(struct vm_bank *bank, uint32_t budget_max) {
    if (!bank || !bank->running)
        return 0;

    uint32_t budget = budget_max < bank->budget ? budget_max : bank->budget;
    uint32_t steps  = 0;

    while (steps < budget) {
        if (!vm_step(bank)) { steps++; break; }
        steps++;
        if (!bank->running) break;
    }

    if (bank->running && steps == budget)
        bank->state.last_fault = VM_FAULT_BUDGET;

    bank->budget = (bank->budget > steps) ? (bank->budget - steps) : 0;

    log_write("       vm_run(budget=%u,steps=%u,fault=%d,PC=0x%04x)\n",
              (unsigned)bank->budget, (unsigned)steps,
              (int)bank->state.last_fault, (unsigned)bank->state.PC);
    return steps;
}

/* service */

enum vm_fault vm_service_run(const uint8_t *payload, size_t len) {
    /* envelope: magic(4) + header(2) + stream */
    if (!payload || len < 6)
        return VM_FAULT_RANGE;

    uint16_t m0 = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    uint16_t m1 = (uint16_t)(payload[2] | ((uint16_t)payload[3] << 8));
    if (m0 != VM_M0 || m1 != VM_M1)
        return VM_FAULT_RANGE;

    /* header at payload[4..5], reserved for now */
    /* uint16_t hdr = (uint16_t)(payload[4] | ((uint16_t)payload[5] << 8)); */

    if (g_bank_in_use)
        return VM_FAULT_RANGE;          /* one foreground transaction at a time */

    g_bank_in_use = true;
    vm_bank_init(&g_bank, 0, VM_BUDGET_MAX);
    g_bank.base = 0x0200;               /* fixed base for the baseline */

    enum vm_fault rv;
    if (!vm_bank_load(&g_bank, payload + 6, len - 6)) {
        rv = VM_FAULT_RANGE;
    } else {
        vm_run(&g_bank, VM_BUDGET_MAX);
        rv = g_bank.state.last_fault;
    }

    g_bank_in_use = false;
    return rv;
}

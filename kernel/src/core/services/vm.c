#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <core/version.h>
#include <core/services/vm.h>
#include <core/services/logger.h>

/* syscall table */
struct vm_syscall_slot {
    uint8_t       class;
    uint16_t      id;
    vm_syscall_fn fn;
    const char   *name;
};

static struct vm_syscall_slot g_sys[VM_SYSCALL_MAX];
static uint32_t               g_sys_count;
static bool                   g_busy;       /* one run at a time */

/* machine state for one run */
struct vm {
    const uint8_t *code;        /* image, executed in place */
    uint16_t       len;
    uint8_t        class;       /* syscall namespace, set by the kernel */
    bool           running;
    enum vm_fault  fault;
    uint16_t       R[16];
    uint16_t       I;           /* reserved */
    uint16_t       PC;
    uint16_t       SP;
    uint16_t       FLAGS;
    uint16_t       stack[16];
};

_Static_assert(VM_FAULT_SYSCALL == 7, "vm: vm_fault_name table out of step with enum vm_fault");

const char *vm_fault_name(enum vm_fault f) {
    static const char *const names[] = {
        "ok", "opcode", "pc", "stack", "budget", "envelope", "busy", "syscall"
    };
    return (unsigned)f < sizeof names / sizeof names[0] ? names[f] : "?";
}

/* syscalls */

static vm_syscall_fn sys_lookup(uint8_t class, uint16_t id) {
    for (uint32_t i = 0; i < g_sys_count; i++) {
        if (g_sys[i].class == class && g_sys[i].id == id)
            return g_sys[i].fn;
    }
    return NULL;
}

bool vm_syscall_register(uint8_t class, uint16_t id, vm_syscall_fn fn, const char *name) {
    if (!fn || !name || g_busy)             return false;
    if (g_sys_count >= VM_SYSCALL_MAX)      return false;
    if (sys_lookup(class, id) != NULL)      return false;

    g_sys[g_sys_count].class = class;
    g_sys[g_sys_count].id    = id;
    g_sys[g_sys_count].fn    = fn;
    g_sys[g_sys_count].name  = name;
    g_sys_count++;
    return true;
}

/* fetch: PC is a logical address, the image starts at VM_ORG */

static bool fetch(const struct vm *m, uint16_t off, uint8_t *out) {
    uint32_t a = (uint32_t)m->PC + off;
    if (a < VM_ORG) return false;
    a -= VM_ORG;
    if (a >= m->len) return false;
    *out = m->code[a];
    return true;
}

static bool fetch_addr(const struct vm *m, uint16_t off, uint16_t *out) {
    uint8_t hi, lo;
    if (!fetch(m, off, &hi) || !fetch(m, (uint16_t)(off + 1), &lo)) return false;
    *out = (uint16_t)(((uint16_t)hi << 8) | lo);
    return true;
}

static bool target_ok(const struct vm *m, uint16_t addr) {
    return addr >= VM_ORG && (uint32_t)(addr - VM_ORG) < m->len;
}

/* one instruction */

static void vm_step(struct vm *m) {
    uint16_t adv = 1;
    uint8_t  op;

    if (!fetch(m, 0, &op)) goto fault_pc;

    switch (op) {

    case VM_OP_NOP:
        break;

    /* register-register */
    case VM_OP_MOV: case VM_OP_ADD: case VM_OP_SUB: case VM_OP_AND:
    case VM_OP_OR:  case VM_OP_XOR: case VM_OP_CMP: {
        uint8_t a, b;
        if (!fetch(m, 1, &a) || !fetch(m, 2, &b)) goto fault_pc;
        if (a > 15 || b > 15) goto fault_op;
        const uint16_t x = m->R[a], y = m->R[b];
        switch (op) {
        case VM_OP_MOV: m->R[a] = y;                    break;
        case VM_OP_ADD: m->R[a] = (uint16_t)(x + y);    break;
        case VM_OP_SUB: m->R[a] = (uint16_t)(x - y);    break;
        case VM_OP_AND: m->R[a] = (uint16_t)(x & y);    break;
        case VM_OP_OR:  m->R[a] = (uint16_t)(x | y);    break;
        case VM_OP_XOR: m->R[a] = (uint16_t)(x ^ y);    break;
        default: /* CMP */
            if (x == y) m->FLAGS |= VM_FLAG_Z;
            else        m->FLAGS &= (uint16_t)~VM_FLAG_Z;
            break;
        }
        adv = 3;
        break;
    }

    /* register-immediate */
    case VM_OP_LDI: case VM_OP_SHL: case VM_OP_SHR: {
        uint8_t a, imm;
        if (!fetch(m, 1, &a) || !fetch(m, 2, &imm)) goto fault_pc;
        if (a > 15) goto fault_op;
        switch (op) {
        case VM_OP_LDI: m->R[a] = imm;                                       break;
        case VM_OP_SHL: m->R[a] = (uint16_t)(m->R[a] << (imm & 15));         break;
        default: /* SHR */
                        m->R[a] = (uint16_t)(m->R[a] >> (imm & 15));         break;
        }
        adv = 3;
        break;
    }

    /* branching: the target is validated even when the branch is not taken */
    case VM_OP_JMP: case VM_OP_JZ: case VM_OP_JNZ: case VM_OP_CALL: {
        uint16_t tgt;
        if (!fetch_addr(m, 1, &tgt) || !target_ok(m, tgt)) goto fault_pc;
        const bool z = (m->FLAGS & VM_FLAG_Z) != 0;
        bool take = true;
        if (op == VM_OP_JZ)  take = z;
        if (op == VM_OP_JNZ) take = !z;
        if (op == VM_OP_CALL) {
            if (m->SP >= 16) goto fault_stack;
            m->stack[m->SP++] = (uint16_t)(m->PC + 3);
        }
        if (take) { m->PC = tgt; return; }
        adv = 3;
        break;
    }

    case VM_OP_RET:
        if (m->SP == 0) goto fault_stack;
        m->PC = m->stack[--m->SP];
        return;

    case VM_OP_SYS: {
        uint16_t id;
        if (!fetch_addr(m, 1, &id)) goto fault_pc;
        vm_syscall_fn fn = sys_lookup(m->class, id);
        if (!fn || !fn(m->R)) goto fault_sys;
        adv = 3;
        break;
    }

    case VM_OP_HALT:
        m->PC = (uint16_t)(m->PC + 1);
        m->running = false;
        return;

    default:
        goto fault_op;
    }

    m->PC = (uint16_t)(m->PC + adv);
    return;

fault_sys:   m->fault = VM_FAULT_SYSCALL; m->running = false; return;
fault_stack: m->fault = VM_FAULT_STACK;   m->running = false; return;
fault_pc:    m->fault = VM_FAULT_PC;      m->running = false; return;
fault_op:    m->fault = VM_FAULT_OPCODE;  m->running = false; return;
}

/* service */

enum vm_fault vm_service_run(uint8_t class, const uint8_t *payload, size_t len) {
    if (!payload || len <= VM_ENVELOPE || len > ROUTER_MAX_PAYLOAD)
        return VM_FAULT_ENVELOPE;
    if (payload[0] != 'V' || payload[1] != 'M' || payload[2] != '1' || payload[3] != '6')
        return VM_FAULT_ENVELOPE;
    if (payload[4] != 0 || payload[5] != 0)     /* header is reserved, must be zero */
        return VM_FAULT_ENVELOPE;
    if (g_busy)
        return VM_FAULT_BUSY;

    g_busy = true;

    struct vm m = {
        .code    = payload + VM_ENVELOPE,
        .len     = (uint16_t)(len - VM_ENVELOPE),
        .class   = class,
        .running = true,
        .fault   = VM_OK,
        .PC      = VM_ORG,
    };

    uint32_t steps = 0;
    while (m.running && steps < VM_BUDGET_MAX) {
        vm_step(&m);
        steps++;
    }
    if (m.running) {                            /* budget spent: end the run */
        m.fault   = VM_FAULT_BUDGET;
        m.running = false;
    }

    log_write("       vm_run(steps=%u,fault=%s,PC=0x%04x)\n",
              (unsigned)steps, vm_fault_name(m.fault), (unsigned)m.PC);

    g_busy = false;
    return m.fault;
}

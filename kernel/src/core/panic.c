#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <core/cmd.h>
#include <core/cpu.h>
#include <core/ext.h>
#include <core/isr.h>
#include <core/kprintf.h>
#include <core/panic.h>
#include <core/vmm.h>

#define KERNEL_HALF 0xffff800000000000ULL   /* kernel addresses live above this */
#define MAX_FRAMES  16

#define U(x) ((unsigned long long)(x))

/* A panic inside the panic handler would loop forever; the second one just stops. */
static volatile bool panicking;

/* Can 16 bytes at addr (a saved rbp/return-address pair) be read without faulting?
 * A fault inside the crash reporter would cut the report short,
 * so the backtrace checks before it dereferences. */
static bool readable(uint64_t addr) {
    if ((addr & 7) || addr < KERNEL_HALF)
        return false;
    if (!vmm_ready())
        return true;        /* still on the bootloader's tables: best effort */
    return vmm_translate(addr) != 0 && vmm_translate(addr + 8) != 0;
}

/* Walk the saved frame pointers.
 * Works because the build keeps frame pointers (-fno-omit-frame-pointer).
 * Each frame is [saved rbp][return address]. `rip` (if non-zero) is printed first as the faulting instruction. */
static void backtrace(uint64_t rip, uint64_t rbp) {
    kprintf("backtrace:\n");
    int n = 0;
    if (rip)
        kprintf("  #%-2d %p  <- faulting instruction\n", n++, (void *)rip);

    uint64_t *fp = (uint64_t *)rbp;
    for (; n < MAX_FRAMES; n++) {
        if (!readable((uint64_t)(uintptr_t)fp))
            break;
        uint64_t ret = fp[1];
        if (ret == 0)
            break;
        kprintf("  #%-2d %p\n", n, (void *)ret);
        uint64_t *next = (uint64_t *)fp[0];
        if (next <= fp)         /* the stack grows down, so frames must go up */
            break;
        fp = next;
    }
    kprintf("resolve addresses:  addr2line -e kernel/bin-x86_64/symbiote <addr>\n");
}

/* Who was running: tells you who to blame. */
static void print_blame(void) {
    const char *ext = ext_current();
    if (ext)
        kprintf("while initialising extension: %s\n", ext);
    const char *cmd = cmd_current();
    if (cmd)
        kprintf("while running command: %s\n", cmd);
}

void panic_at(const char *file, int line, const char *fmt, ...) {
    cpu_cli();

    if (panicking)
        cpu_halt_forever();
    panicking = true;

    uint64_t rsp, rbp, rflags, cr2, cr3;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    kprintf("\n\x1b[1;91m*** SYMBIOTE PANIC ***\x1b[0m\n");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\nat %s:%d\n", file, line);

    print_blame();

    kprintf("rsp=%p rbp=%p rflags=%llx\n", (void *)rsp, (void *)rbp, U(rflags));
    kprintf("cr2=%p cr3=%p\n", (void *)cr2, (void *)cr3);
    backtrace(0, rbp);
    kprintf("\nSystem halted.\n");

    cpu_halt_forever();
}

/*  CPU exceptions  */

/* #TS, #NP, #SS and #GP report a selector in the error code. */
static void describe_selector_error(uint64_t err) {
    if (err == 0) {
        kprintf("  error code 0: not caused by a segment selector (bad address, "
                "privileged instruction, or non-canonical access)\n");
        return;
    }
    const char *table = (err & 2) ? "IDT" : (err & 4) ? "LDT" : "GDT";
    kprintf("  selector error: %s index %llu%s\n", table, U(err >> 3),
            (err & 1) ? ", caused by an external event" : "");
}

static void describe_page_fault(uint64_t err, uint64_t cr2) {
    const char *access = (err & 0x10) ? "instruction fetch" : (err & 0x2) ? "write" : "read";
    const char *cause  = (err & 0x8)  ? "reserved bit set in a page-table entry"
                       : (err & 0x1)  ? "protection violation (page is present)"
                                      : "page not present";
    kprintf("  %s at %p: %s%s\n", access, (void *)cr2, cause,
            (err & 0x4) ? ", from user mode" : "");

    if (!vmm_ready()) {
        kprintf("  vmm not ready: still on the bootloader's page tables\n");
    } else {
        uint64_t phys = vmm_translate(cr2);
        if (phys)
            kprintf("  vmm: %p is mapped to physical %p\n", (void *)cr2, (void *)phys);
        else
            kprintf("  vmm: %p is not mapped\n", (void *)cr2);
    }
}

void panic_exception(const struct isr_frame *f) {
    cpu_cli();

    if (panicking)
        cpu_halt_forever();
    panicking = true;

    uint64_t cr2 = read_cr2();
    uint64_t cr3 = read_cr3();
    int v = (int)f->vector;

    kprintf("\n\x1b[1;91m*** SYMBIOTE PANIC ***\x1b[0m\n");
    if (v < 32) {
        kprintf("CPU exception %d: %s\n", v, isr_exception_name(v));
        if (isr_has_error_code(v))
            kprintf("error code 0x%llx\n", U(f->error));
    } else {
        kprintf("unexpected interrupt vector %d: no handler is installed for it\n", v);
    }

    switch (v) {
    case 0:
        kprintf("  division by zero, or the quotient did not fit the register\n");
        break;
    case 3:
        kprintf("  int3 executed\n");
        break;
    case 6:
        kprintf("  invalid opcode (ud2, or the CPU is executing data)\n");
        break;
    case 8:
        kprintf("  the CPU failed while delivering an exception: usually a kernel stack\n"
                "  overflow or a corrupt stack pointer (rsp at the time: %p)\n",
                (void *)f->rsp);
        break;
    case 10: case 11: case 12: case 13:
        describe_selector_error(f->error);
        break;
    case 14:
        describe_page_fault(f->error, cr2);
        break;
    default:
        break;
    }

    print_blame();

    kprintf("rip=%016llx cs=%04llx rflags=%016llx\n", U(f->rip), U(f->cs), U(f->rflags));
    kprintf("rsp=%016llx ss=%04llx\n", U(f->rsp), U(f->ss));
    kprintf("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", U(f->rax), U(f->rbx), U(f->rcx), U(f->rdx));
    kprintf("rsi=%016llx rdi=%016llx rbp=%016llx r8 =%016llx\n", U(f->rsi), U(f->rdi), U(f->rbp), U(f->r8));
    kprintf("r9 =%016llx r10=%016llx r11=%016llx r12=%016llx\n", U(f->r9), U(f->r10), U(f->r11), U(f->r12));
    kprintf("r13=%016llx r14=%016llx r15=%016llx\n", U(f->r13), U(f->r14), U(f->r15));
    kprintf("cr2=%016llx cr3=%016llx\n", U(cr2), U(cr3));

    backtrace(f->rip, f->rbp);
    kprintf("\nSystem halted.\n");

    cpu_halt_forever();
}

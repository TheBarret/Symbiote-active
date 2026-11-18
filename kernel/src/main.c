#include <stddef.h>
#include <stdint.h>

/* kernel core */
#include <core/version.h>           // Master version, includes master config.h
#include <core/arch/boot.h>         // Limine API vectors and helpers
#include <core/arch/cpu.h>          // CPU tables, CPUID, TSC-clock
#include <core/arch/serial.h>       // Serial sink
#include <core/arch/gdt.h>          // Global descriptor table, ring 0
#include <core/arch/idt.h>          // Interrupt Descriptor Table, 256 vectors
#include <core/arch/pic.h>          // 8259 PIC Driver, virtual wires (LAPIC), interrupt platform
#include <core/arch/isr.h>          // Interrupt Service Routine (ISR), isr_stubs.S stub wrapper
#include <core/arch/pmm.h>          // Physical memory manager
#include <core/arch/vmm.h>          // Virtual memory manager, ownership with PMM-backed frames
#include <core/arch/heap.h>         // Block headers, Immediate coalescing, pmm_alloc_contig via HHDM
/* kernel devices */
#include <core/dev/keyboard.h>   // Keyboard device
#include <core/dev/console.h>   // Console device, ps/2
#include <core/dev/vfs.h>       // Virtual File System, ramfs
#include <core/dev/timer.h>     // Clock controller
/* kernel services */
#include <core/services/loop.h> // shell loop manager
#include <core/services/symbol.h> // symbol storage
#include <core/services/random.h>   // randomizer, seed based xorshift16
#include <core/services/router.h>   // miniature datagram network
/* kernel modules */
#include <core/kprintf.h>   // Formatted output to every console
#include <core/klog.h>      // Kernel logger extension (kern, info, warn, error)
#include <core/ext.h>       // Extension-module dispatcher
#include <core/cmd.h>       // Command-line dispatcher
#include <core/shell.h>     // Shell Manager (ref: cmd.h)


/* Loop.h back-end */

static uint32_t run_drain(void *ctx, uint32_t budget) {
    (void)ctx; (void)budget;
    timer_drain();
    return 0;
}
static uint32_t run_input(void *ctx, uint32_t budget) {
    (void)ctx;
    uint32_t n = 0;
    char c = 0;
    while (n < budget && kbd_poll(&c) == KBD_CHAR) {
        shell_tick(c);
        n++;
    }
    return n;
}

static struct task g_tasks[] = {
    { .name = "DRAIN", .run = run_drain, .budget = 1,  .period = 1 },
    { .name = "INPUT", .run = run_input, .budget = 2, .period = 1 },
};

static bool machine_can_run(void) {
    for (size_t i = 0; i < sizeof g_tasks / sizeof g_tasks[0]; i++)
        if (!loop_add(&g_tasks[i])) return false;
    return true;
}

/* boot->kmain() entrypoint */
void kmain(void) {
    // Stage 1 ---------------------------------------------------------------------------
    // Limine validation logic or hang forever
    if (!boot_protocol_ok()) { cpu_halt_forever(); }
    /* essentials */
    klog_init();                // kernel format logger
    /* greeter */
    klog_info("Loading kernel: %s %s-%s...\n", SYM_BOOTNAME, SYM_VERSION, SYM_VERSION_INFO);
    /* kernel modules start */
    gdt_init();                 // CPU tables (no heap required)
    idt_init();
    pmm_init();                 // memory
    heap_init();                // memory
    vmm_init();                 // memory
    pic_init();                 // interrupts
    timer_init(TIMER_HZ);       // interrupts
    cpu_sti();                  // interrupts
    serial_init();              // serial sink
    // Stage 2 ---------------------------------------------------------------------------
    if (!kbd_init()) {          // need keyboard
        klog_error("no PS/2 keyboard detected, halting...\n");
        cpu_halt_forever();     // no keyboard, halt.
    }
    vfs_init();                 // filesystem storage
    /* services */
    rnd_init(RND_SEED);         // randomizer
    router_init();              // virtual bus
    symbol_init();              // symbol storage
    /* kernel modules end */
    klog_info("Stage 1 → Stage 2\n");
    /* stage two: extensions */
    ext_init_all();             // extensions
    cmd_selfcheck();            /* commence self-test after ext-registration */
    // Stage 3 ---------------------------------------------------------------------------
    /* greeter */
    klog_info("Stage 2 complete (ext: %zu, sys: %zu)\n", ext_count(), cmd_count());
    shell_init();               // init shell dispatcher
    kprintf("\nType 'help' for a list of command.\n");

    if (!machine_can_run()) {
            klog_error("task registration failed, halting...\n");
            cpu_halt_forever();
        }
    loop_run(cpu_sti_hlt);      /* noreturn */
}

#include <stddef.h>
#include <stdint.h>

/* kernel core */
#include <core/version.h>           // Master version, includes master config.h
#include <core/arch/boot.h>         // Limine API vectors and helpers
#include <core/arch/cpu.h>          // CPU tables, CPUID, TSC-clock
#include <core/arch/serial.h>       // Serial driver, transmits to COM1 at 115200 baud
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
#include <core/dev/env.h>       // Environment variable store device
#include <core/dev/metrics.h>   // Metrics and stats servicer

/* kernel modules */
#include <core/bus.h>       // Bus registry, late device discovery (ref: kernel devices)
#include <core/kprintf.h>   // Formatted output to every console
#include <core/klog.h>      // Kernel logger extension (kern, info, warn, error)
#include <core/ext.h>       // Extension-module dispatcher
#include <core/cmd.h>       // Command-line dispatcher
#include <core/shell.h>     // Shell Manager (ref: cmd.h)

void kmain(void) {
    // ST1 ---------------------------------------------------------------------------
    // Limine validation logic or hang forever
    if (!boot_protocol_ok()) { cpu_halt_forever(); }
    /* essentials */
    serial_init();              // serial output (read-only)
    klog_init();                // kernel format logger
    bus_init();                 // load bus registry
    /* greeter */
    klog_info("Loading kernel: Symbiote %s-%s...\n", SYM_VERSION, SYM_VERSION_INFO);
    /* kernel modules start */
    console_init();             // console writer
    gdt_init();                 // CPU tables (no heap required)
    idt_init();
    pmm_init();                 // memory
    heap_init();                // memory
    vmm_init();                 // memory
    pic_init();                 // interrupts
    timer_init(TIMER_HZ);       // interrupts
    cpu_sti();                  // interrupts

    // ST2 ---------------------------------------------------------------------------
    env_init(0,0);              // environment must be first, (0 = default)

    if (!kbd_init()) {          // keyboard
        klog_error("Error: no PS/2 keyboard detected!\n");
        cpu_halt_forever();     // no keyboard, halt.
    }
    vfs_init();                 // filesystem
    metrics_init(METRIC_MS);    // metric service

    /* kernel modules end */
    klog_info("Stage 1 → Stage 2\n");

    /* stage two: extensions */
    ext_init_all();             // extensions
    cmd_selfcheck();            /* catch command-spec typos at boot, after ext registration */

    bus_settle();               // write device manifest to env

    // ST3 ---------------------------------------------------------------------------
    /* greeter */
    klog_info("Stage 2 complete (ext: %zu, sys: %zu)\n", ext_count(), cmd_count());

    shell_init();               // init shell dispatcher
    kprintf("\nType 'help' for a list of commands, ready when you are!\n");

    for (;;) {
        /* drain any scheduled timers (env sweep, top refresh, vm16 async, etc.) */
        timer_drain();      // do not move this!
        /* poll for keyboard input without blocking */
        char c = 0;
        enum kbd_event ev = kbd_poll(&c);
        if (ev == KBD_CHAR) {
            shell_tick(c);
        }
        /* sleep until the next interrupt (timer or keyboard) */
        cpu_sti_hlt();
    }

    /* shouldn't happen but just in case, to avoid fallthroughs */
    cpu_halt_forever();

}

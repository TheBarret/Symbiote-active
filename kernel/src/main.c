#include <stddef.h>
#include <stdint.h>

/* kernel core */
#include <core/version.h>           // Master version, includes master config.h
#include <core/arch/boot.h>         // Limine API vectors and helpers
#include <core/arch/cpu.h>          // CPU tables, CPUID, TSC-clock
#include <core/arch/serial.h>       // Serial driver, read-only, default is COM1, 115200 baud
#include <core/arch/gdt.h>          // Global descriptor table, ring 0
#include <core/arch/idt.h>          // Interrupt Descriptor Table, 256 vectors
#include <core/arch/pic.h>          // 8259 PIC Driver, virtual wires (LAPIC), interrupt platform
#include <core/arch/isr.h>          // Interrupt Service Routine (ISR), isr_stubs.S stub wrapper
#include <core/arch/pmm.h>          // Physical memory manager
#include <core/arch/vmm.h>          // Virtual memory manager, ownership with PMM-backed frames
#include <core/arch/heap.h>         // Block headers, Immediate coalescing, pmm_alloc_contig via HHDM

/* kernel devices */
#include <core/dev/console.h>   // Console device, ps/2, serial
#include <core/dev/vfs.h>       // Virtual File System, ramfs
#include <core/dev/timer.h>     // Clock controller
#include <core/dev/env.h>       // Environment variable store device

/* kernel modules */
#include <core/bus.h>       // Bus registry, late device discovery
#include <core/kprintf.h>   // Formatted output to every console
#include <core/klog.h>      // Kernel logger extension (kern, info, warn, error)
#include <core/ext.h>       // Extension-module dispatcher
#include <core/cmd.h>       // Command-line dispatcher
#include <core/shell.h>     // Shell Manager (ref: cmd.h)

void kmain(void) {
    // Limine validation logic or hang forever
    if (!boot_protocol_ok()) {
        cpu_halt_forever();
    }

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
    //struct pmm_stats pm = pmm_get_stats();
    //klog_info("PMM: (%zu MiB usable)\n", (pm.free_frames * PMM_PAGE_SIZE) / (1024 * 1024));

    heap_init();                // memory
    //struct heap_stats hs = heap_get_stats();
    //klog_info("HEAP: (%zu KiB free)\n", hs.bytes_free / 1024);

    vmm_init();                 // memory

    pic_init();                 // interrupts
    timer_init(TIMER_HZ);       // interrupts
    cpu_sti();                  // interrupts

    vfs_init();                 // filesystem

    env_init(0,0);                 // environment (0 = default)

    /* kernel modules end */
    klog_info("Stage 1 → Stage 2\n");

    /* stage two: extensions */
    ext_init_all();             // extensions

    /* greeter */
    klog_info("Stage 2 complete (ext: %zu, sys: %zu)\n", ext_count(), cmd_count());

    /* launch shell */
    shell_run();
}

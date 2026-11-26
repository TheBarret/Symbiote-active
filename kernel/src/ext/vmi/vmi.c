#include <stdbool.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/version.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/kprintf.h>
#include <core/klog.h>
#include <core/dev/vfs.h>
#include <core/services/addr.h>
#include <core/services/router.h>
#include <core/services/vm.h>
#include <core/services/logger.h>

/* VMI - front-end of the vm16.h */

static int cmd_run_fn(const struct cmd_args *a) {
    /* a->argv[0] is the path, validated by the dispatcher as :str */
    const char *path = a->argv[0];

    /* read the ROM image from VFS */
    struct vfs_file *f = vfs_open(path, VFS_O_RDONLY);
    if (!f) {
        klog_error("vm: cannot open '%s'\n", path);
        return CMD_ERR;
    }

    static uint8_t rom[ROUTER_SIZE_MAX];
    long n = vfs_read(f, rom, sizeof rom);
    vfs_close(f);

    if (n <= 0) {
        klog_error("vm: empty or unreadable '%s'\n", path);
        return CMD_ERR;
    }

    /* route signal frame */
    router_frame *pkt = router_create_p(ADDR_UNICAST(BUS_VM), BUS_VM,
                                        FRAME_SIGNAL, VM_PORT,
                                        rom, (uint16_t)n);
    if (!pkt) {
        klog_error("vm: invalid frame (path=%s)\n", path);
        return CMD_ERR;
    }

    /* send; router takes ownership */
    if (!router_send(pkt)) {
        klog_error("vm: router rejected frame\n");
        return CMD_ERR;
    }

    return CMD_OK;
}


/* ------------------------------------------------------------------------- */

static const uint8_t rom_test[] = {
    0x56, 0x4D, 0x31, 0x36, /* header */
    0x00, 0x00,             /* flags */
    0x02, 0x00, 0xAB,       /* LDI R0, 0xAB */
    0x20, 0x00, 0x00,       /* call 0x0000, R0 */
    0x0F,                   /* HALT */
};


static int cmd_vmtest_fn(const struct cmd_args *a) {
    (void)a;
    router_frame *pkt = router_create_p(ADDR_UNICAST(NOBODY), ADDR_UNICAST(BUS_VM),
                                        FRAME_SIGNAL, DEFAULT_PORT,
                                        rom_test, sizeof rom_test);
    if (!pkt) return CMD_ERR;
    return router_send(pkt) ? CMD_OK : CMD_ERR;
}

static bool syscall_test(struct vm_bank *bank) {
    klog("VM test:%s\n", ((unsigned)bank->state.R[0] == 0xAB) ? "...[OK]" : "...[FAILED]");
    klog("0x0000: R0=0x%04x, R1=0x%04x, R2=0x%04x, R3=0x%04x, exit=0x%04x\n",
        (unsigned)bank->state.R[0], (unsigned)bank->state.R[1],
        (unsigned)bank->state.R[2], (unsigned)bank->state.R[3],
        (unsigned)bank->state.last_fault);
    return true;
}
SYM_COMMAND(vmtest, "", "Perform VM test", cmd_vmtest_fn);
/* ------------------------------------------------------------------------- */

/* vm frame validation */
static bool vm_valid(router_frame *pkt) {
    if (pkt->length < 6) { return false; }
    uint16_t m0 = (uint16_t)(pkt->payload[0] | (pkt->payload[1] << 8));
    uint16_t m1 = (uint16_t)(pkt->payload[2] | (pkt->payload[3] << 8));
    return (m0 != VM_M0 || m1 != VM_M1);
}

/* vm receiver */
static void vm_rx(router_frame *pkt) {
    /* validate */
    if (!pkt) { return; }
    if (pkt->type != FRAME_SIGNAL || pkt->port == DEFAULT_BC) { return; }
    if (!vm_valid(pkt)) { return; }

    log_write("       vm_rx(%u) accepted frame\n", (unsigned)pkt->sentinel);

    static struct vm_bank bank;
    vm_bank_init(&bank, 0, VM_BUDGET_MAX);

    //static size_t steps = 0;
    bank.base = 0x0200;                 /* fixed base 512 for now */
    bank.end  = VM_MEM_SIZE;
    if (!vm_bank_load(&bank, pkt->payload + 6, pkt->length - 6))
        return;
    //steps = vm_run(&bank, VM_BUDGET_MAX);
    vm_run(&bank, VM_BUDGET_MAX);
}

/* vm init */
static int vmi_init(void) {
    if (router_register("vmi", BUS_VM, vm_rx)) {
        vm_syscall_register(0, 0x0000, syscall_test, "sc_test");
        return 0;
    }
    return  1;
}
SYM_EXTENSION(vmi_control, vmi_init, EXT_PRIO_APPLET);
SYM_COMMAND(load, "<path:str>", "Load ROM program", cmd_run_fn);

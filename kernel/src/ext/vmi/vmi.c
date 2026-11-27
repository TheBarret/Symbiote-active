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

/* VMI: shell front-end of the VM16 service on the router bus */

/* result of the last run, read back by vm_submit() after router_send() */
static bool          g_ran;
static enum vm_fault g_fault = VM_OK;

/* bus receiver: SIGNAL frames on VM_PORT addressed to the VM itself.
 * Casts (broadcast, multicast) also reach this handler, so dst is checked.
 */
static void vm_rx(router_frame *pkt) {
    if (!pkt) return;
    if (pkt->type != FRAME_SIGNAL || pkt->port != VM_PORT) return;
    if (pkt->dst_addr != ADDR_UNICAST(BUS_VM)) return;

    /* kernel-assigned syscall class */
    g_fault = vm_service_run(0, pkt->payload, pkt->length);
    g_ran   = true;
    log_write("       vm_rx(%u) fault=%s\n", (unsigned)pkt->sentinel, vm_fault_name(g_fault));
}

/* send an image to the VM and report the outcome */
static int vm_submit(const uint8_t *img, uint16_t n, const char *what) {
    router_frame *pkt = router_create_p(ADDR_UNICAST(NOBODY), ADDR_UNICAST(BUS_VM),
                                        FRAME_SIGNAL, VM_PORT, img, n);
    if (!pkt) {
        klog_error("vm: invalid frame (%s)\n", what);
        return CMD_ERR;
    }

    g_ran = false;
    if (!router_send(pkt)) {            /* router takes ownership */
        klog_error("vm: router rejected frame (%s)\n", what);
        return CMD_ERR;
    }
    if (!g_ran) {
        klog_error("vm: frame ignored (%s)\n", what);
        return CMD_ERR;
    }
    if (g_fault != VM_OK) {
        klog_error("vm: %s faulted: %s\n", what, vm_fault_name(g_fault));
        return CMD_ERR;
    }
    return CMD_OK;
}

static int cmd_run_fn(const struct cmd_args *a) {
    const char *path = a->argv[0];      /* validated by the dispatcher as :str */

    struct vfs_file *f = vfs_open(path, VFS_O_RDONLY);
    if (!f) {
        klog_error("vm: cannot open '%s'\n", path);
        return CMD_ERR;
    }

    /* one byte more than the limit, so an oversized file is detected, not truncated */
    static uint8_t rom[ROUTER_MAX_PAYLOAD + 1];
    size_t got = 0;
    long   r;
    while (got < sizeof rom && (r = vfs_read(f, rom + got, sizeof rom - got)) > 0)
        got += (size_t)r;
    vfs_close(f);

    if (got == 0) {
        klog_error("vm: empty or unreadable '%s'\n", path);
        return CMD_ERR;
    }
    if (got > ROUTER_MAX_PAYLOAD) {
        klog_error("vm: '%s' too large (max %zu)\n", path, (size_t)ROUTER_MAX_PAYLOAD);
        return CMD_ERR;
    }
    return vm_submit(rom, (uint16_t)got, path);
}

/* ------------------------------------------------------------------------- */

static const uint8_t rom_test[] = {
    'V', 'M', '1', '6',     /* magic */
    0x00, 0x00,             /* header, reserved */
    0x02, 0x00, 0xAB,       /* LDI R0, 0xAB */
    0x20, 0x00, 0x00,       /* SYS 0x0000 */
    0x0F,                   /* HALT */
};

static int cmd_vmtest_fn(const struct cmd_args *a) {
    (void)a;
    return vm_submit(rom_test, sizeof rom_test, "vmtest");
}

static bool syscall_test(uint16_t *R) {
    klog("VM test:%s\n", R[0] == 0xAB ? "...[OK]" : "...[FAILED]");
    klog("0x0000: R0=0x%04x, R1=0x%04x, R2=0x%04x, R3=0x%04x\n",
         (unsigned)R[0], (unsigned)R[1], (unsigned)R[2], (unsigned)R[3]);
    return true;
}
SYM_COMMAND(vmtest, "", "Perform VM test", cmd_vmtest_fn);
/* ------------------------------------------------------------------------- */

static int vmi_init(void) {
    if (!router_register("vmi", ADDR_UNICAST(BUS_VM), vm_rx))
        return 1;
    vm_syscall_register(0, 0x0000, syscall_test, "sc_test");
    return 0;
}
SYM_EXTENSION(vmi, vmi_init, EXT_PRIO_APPLET);
SYM_COMMAND(load, "<path:str>", "Load ROM program", cmd_run_fn);

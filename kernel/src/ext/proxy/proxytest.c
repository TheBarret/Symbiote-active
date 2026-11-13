#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/dev/env.h>
#include <core/bus.h>
#include <core/kprintf.h>
#include <core/cmd.h>
#include <core/ext.h>
#include <core/mask.h>
#include <ext/proxy/proxy.h>

/* proxytest: wire-format and boundary tests for the proxy device.
 *
 * Runs from the shell. No TUI, no interaction.
 * Each test is a self-contained bind/call/unbind cycle;
 * failures print a specific line, then the summary prints pass/fail counts.
 *
 * Filter: `proxytest <prefix>` runs only tests whose name starts with
 * <prefix>. Prefixes in use: "console.", "env.", "misc.".
 *
 * The tests exercise the proxy's public API (bind/call/unbind),
 * never the underlying env/console ops directly.
 * If a test needs to bypass the proxy for setup (e.g. to plant an entry with a foreign owner),
 * it goes through bus_env() explicitly and the test name says so. */

/*
 * helpers
 *  */

/* Pack a NUL-terminated string into the wire buffer. Returns the new
 * position, or SIZE_MAX on overflow. */
static size_t pack_str(uint8_t *buf, size_t cap, size_t pos, const char *s) {
    size_t n = strlen(s) + 1;
    if (pos + n > cap) return SIZE_MAX;
    memcpy(buf + pos, s, n);
    return pos + n;
}

static size_t pack_u64(uint8_t *buf, size_t cap, size_t pos, uint64_t v) {
    if (pos + 8 > cap) return SIZE_MAX;
    memcpy(buf + pos, &v, 8);
    return pos + 8;
}

//static size_t pack_u32(uint8_t *buf, size_t cap, size_t pos, uint32_t v) {
//    if (pos + 4 > cap) return SIZE_MAX;
//    memcpy(buf + pos, &v, 4);
//    return pos + 4;
//}

/* Read a little-endian u64 from a byte buffer. */
static uint64_t unpack_u64(const uint8_t *buf) {
    uint64_t v;
    memcpy(&v, buf, 8);
    return v;
}

static uint32_t unpack_u32(const uint8_t *buf) {
    uint32_t v;
    memcpy(&v, buf, 4);
    return v;
}

/* Convenience: bind a device; returns PROXY_OK on success. */
static int bind_dev(const struct proxy_ops *p, const char *name, uint32_t *h) {
    return p->bind(name, h);
}

/* Build the proxy's usual mask so tests can count/stat what they wrote. */
//static uint32_t proxy_mask(void) {
//    return env_mask_build((uint16_t)BUS_PROXY, ENV_TYPE_STRING, 0, 0);
//}

/*
 * console group
 *  */

static int t_console_bind(const struct proxy_ops *p) {
    uint32_t h = 0xFFFFFFFF;
    int rc = bind_dev(p, "console", &h);
    if (rc != PROXY_OK) {
        kprintf("  console.bind: bind returned %d\n", rc);
        return 1;
    }
    if (h >= 32) {
        kprintf("  console.bind: handle %u out of range\n", h);
        p->unbind(h);
        return 1;
    }
    p->unbind(h);
    return 0;
}

static int t_console_write(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "console", &h) != PROXY_OK) {
        kprintf("  console.write: bind failed\n");
        return 1;
    }

    uint8_t args[64];
    size_t len = pack_str(args, sizeof args, 0, "proxytest: console.write ok\n");
    if (len == SIZE_MAX) { p->unbind(h); return 1; }

    size_t ret_len = 999;
    int rc = p->call(h, PROXY_CONSOLE_WRITE, args, len, NULL, 0, &ret_len);
    p->unbind(h);
    if (rc != PROXY_OK) {
        kprintf("  console.write: call returned %d\n", rc);
        return 1;
    }
    if (ret_len != 0) {
        kprintf("  console.write: ret_len = %zu (expected 0)\n", ret_len);
        return 1;
    }
    return 0;
}

static int t_console_write_bad_arg(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "console", &h) != PROXY_OK) return 1;

    /* No NUL terminator anywhere in the payload. */
    const uint8_t args[] = { 'h', 'i', '!' };
    int rc = p->call(h, PROXY_CONSOLE_WRITE, args, sizeof args, NULL, 0, NULL);
    p->unbind(h);
    if (rc != PROXY_EBADARG) {
        kprintf("  console.write_bad_arg: got %d, expected EBADARG\n", rc);
        return 1;
    }
    return 0;
}

static int t_console_clear_bad_op(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "console", &h) != PROXY_OK) return 1;

    int rc = p->call(h, PROXY_CONSOLE_CLEAR, NULL, 0, NULL, 0, NULL);
    p->unbind(h);
    if (rc != PROXY_EBADOP) {
        kprintf("  console.clear_bad_op: got %d, expected EBADOP\n", rc);
        return 1;
    }
    return 0;
}

static int t_console_unknown_op(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "console", &h) != PROXY_OK) return 1;

    int rc = p->call(h, 0xFFFF, NULL, 0, NULL, 0, NULL);
    p->unbind(h);
    if (rc != PROXY_EBADOP) {
        kprintf("  console.unknown_op: got %d, expected EBADOP\n", rc);
        return 1;
    }
    return 0;
}

static int t_console_stale(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "console", &h) != PROXY_OK) return 1;
    p->unbind(h);

    uint8_t args[8];
    size_t len = pack_str(args, sizeof args, 0, "x");
    int rc = p->call(h, PROXY_CONSOLE_WRITE, args, len, NULL, 0, NULL);
    if (rc != PROXY_ESTALE) {
        kprintf("  console.stale: got %d, expected ESTALE\n", rc);
        return 1;
    }
    return 0;
}

/*
 * env group
 *  */

#define ENV_TEST_NS   "script.proxytest"
#define ENV_TEST_NAME "k1"
#define ENV_TEST_VAL  "hello"

/* Cleanup helper: unset a key via the proxy, ignoring the result. */
static void cleanup_key(const struct proxy_ops *p, uint32_t h,
                        const char *ns, const char *name) {
    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ns);
    pos = pack_str(args, sizeof args, pos, name);
    if (pos == SIZE_MAX) return;
    p->call(h, PROXY_ENV_UNSET, args, pos, NULL, 0, NULL);
}

static int t_env_bind(const struct proxy_ops *p) {
    uint32_t h;
    int rc = bind_dev(p, "env", &h);
    if (rc != PROXY_OK) {
        kprintf("  env.bind: bind returned %d\n", rc);
        return 1;
    }
    p->unbind(h);
    return 0;
}

static int t_env_set_get(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);

    /* SET */
    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_VAL);
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }

    int rc = p->call(h, PROXY_ENV_SET, args, pos, NULL, 0, NULL);
    if (rc != PROXY_OK) {
        kprintf("  env.set_get: set returned %d\n", rc);
        p->unbind(h);
        return 1;
    }

    /* GET */
    pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }

    uint8_t ret[64];
    size_t ret_len = 0;
    rc = p->call(h, PROXY_ENV_GET, args, pos, ret, sizeof ret, &ret_len);
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);
    p->unbind(h);

    if (rc != PROXY_OK) {
        kprintf("  env.set_get: get returned %d\n", rc);
        return 1;
    }
    size_t expect = strlen(ENV_TEST_VAL);
    if (ret_len != expect) {
        kprintf("  env.set_get: ret_len = %zu, expected %zu\n", ret_len, expect);
        return 1;
    }
    if (memcmp(ret, ENV_TEST_VAL, expect) != 0) {
        kprintf("  env.set_get: value mismatch\n");
        return 1;
    }
    return 0;
}

static int t_env_set_ns_gate(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, "kernel.dev");
    pos = pack_str(args, sizeof args, pos, "x");
    pos = pack_str(args, sizeof args, pos, "y");
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }

    int rc = p->call(h, PROXY_ENV_SET, args, pos, NULL, 0, NULL);
    p->unbind(h);
    if (rc != PROXY_EBADOP) {
        kprintf("  env.set_ns_gate: got %d, expected EBADOP\n", rc);
        return 1;
    }
    return 0;
}

static int t_env_is_exist_yes(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_VAL);
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }
    p->call(h, PROXY_ENV_SET, args, pos, NULL, 0, NULL);

    pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    int rc = p->call(h, PROXY_ENV_IS_EXIST, args, pos, NULL, 0, NULL);

    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);
    p->unbind(h);
    if (rc != PROXY_OK) {
        kprintf("  env.is_exist_yes: got %d, expected OK\n", rc);
        return 1;
    }
    return 0;
}

static int t_env_is_exist_no(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    cleanup_key(p, h, ENV_TEST_NS, "nonexistent");

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, "nonexistent");
    int rc = p->call(h, PROXY_ENV_IS_EXIST, args, pos, NULL, 0, NULL);
    p->unbind(h);
    if (rc != PROXY_ENOENT) {
        kprintf("  env.is_exist_no: got %d, expected ENOENT\n", rc);
        return 1;
    }
    return 0;
}

static int t_env_stat(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);

    /* Setup: SET */
    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_VAL);
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }
    p->call(h, PROXY_ENV_SET, args, pos, NULL, 0, NULL);

    /* STAT */
    pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    uint8_t ret[64];
    size_t ret_len = 0;
    int rc = p->call(h, PROXY_ENV_STAT, args, pos, ret, sizeof ret, &ret_len);

    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);
    p->unbind(h);

    if (rc != PROXY_OK) {
        kprintf("  env.stat: got %d, expected OK\n", rc);
        return 1;
    }
    /* Layout: u32 value_len, u32 access_remaining, u64 has_ttl,
     *         u64 ttl_remaining, u64 last_access, u32 importance, u32 mask */
    size_t off = 0;
    uint32_t value_len = unpack_u32(ret + off); off += 4;
    uint32_t access_rem = unpack_u32(ret + off); off += 4;
    uint64_t has_ttl    = unpack_u64(ret + off); off += 8;
    off += 8;   /* ttl_remaining */
    off += 8;   /* last_access    */
    off += 4;   /* importance     */
    uint32_t mask       = unpack_u32(ret + off);

    if (value_len != strlen(ENV_TEST_VAL)) {
        kprintf("  env.stat: value_len = %u, expected %zu\n",
                (unsigned)value_len, strlen(ENV_TEST_VAL));
        return 1;
    }
    if (access_rem != 0) {
        kprintf("  env.stat: access_remaining = %u, expected 0 (unlimited)\n",
                (unsigned)access_rem);
        return 1;
    }
    if (has_ttl != 0) {
        kprintf("  env.stat: has_ttl = %llu, expected 0\n",
                (unsigned long long)has_ttl);
        return 1;
    }
    uint8_t owner = env_mask_owner(mask);
    uint8_t type  = env_mask_type(mask);
    if (owner != BUS_PROXY) {
        kprintf("  env.stat: mask owner = %u, expected %u\n",
                (unsigned)owner, (unsigned)BUS_PROXY);
        return 1;
    }
    if (type != ENV_TYPE_STRING) {
        kprintf("  env.stat: mask type = %u, expected STRING\n", (unsigned)type);
        return 1;
    }
    return 0;
}

static int t_env_stat_missing(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    cleanup_key(p, h, ENV_TEST_NS, "definitely_missing");

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, "definitely_missing");
    uint8_t ret[64];
    size_t ret_len = 999;
    int rc = p->call(h, PROXY_ENV_STAT, args, pos, ret, sizeof ret, &ret_len);
    p->unbind(h);
    if (rc != PROXY_ENOENT) {
        kprintf("  env.stat_missing: got %d, expected ENOENT\n", rc);
        return 1;
    }
    if (ret_len != 0) {
        kprintf("  env.stat_missing: ret_len = %zu, expected 0\n", ret_len);
        return 1;
    }
    return 0;
}

static int t_env_cross_owner_unset(const struct proxy_ops *p) {
    const struct env_ops *e = bus_env();
    if (e == NULL) return 1;

    /* Plant an entry with owner BUS_VM16 directly via env, bypassing the proxy. */
    const char *ns = "script.proxytest";
    const char *nm = "foreign";
    uint32_t plant_mask = env_mask_build((uint16_t)BUS_VM16, ENV_TYPE_STRING, 0, 0);
    if (e->set(ns, nm, "v", 0, 0, 0, plant_mask) != ENV_OK) return 1;

    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) { e->unset(ns, nm); return 1; }

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ns);
    pos = pack_str(args, sizeof args, pos, nm);
    int rc = p->call(h, PROXY_ENV_UNSET, args, pos, NULL, 0, NULL);
    p->unbind(h);

    /* Clean up the planted entry regardless of test outcome. */
    e->unset(ns, nm);

    if (rc != PROXY_EBADOP) {
        kprintf("  env.cross_owner_unset: got %d, expected EBADOP\n", rc);
        return 1;
    }
    return 0;
}

static int t_env_unset_own(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_VAL);
    p->call(h, PROXY_ENV_SET, args, pos, NULL, 0, NULL);

    pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    int rc1 = p->call(h, PROXY_ENV_UNSET, args, pos, NULL, 0, NULL);
    int rc2 = p->call(h, PROXY_ENV_UNSET, args, pos, NULL, 0, NULL);
    p->unbind(h);

    if (rc1 != PROXY_OK) {
        kprintf("  env.unset_own: first unset got %d, expected OK\n", rc1);
        return 1;
    }
    if (rc2 != PROXY_ENOENT) {
        kprintf("  env.unset_own: second unset got %d, expected ENOENT\n", rc2);
        return 1;
    }
    return 0;
}

static int t_env_list_all_end(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;

    /* Iterate to exhaustion. Every entry returned must be non-empty,
     * and the terminal return must be PROXY_ENOENT (not EINTERNAL). */
    uint64_t iter = 0;
    int steps = 0;
    int rc = PROXY_OK;
    while (steps < 2048) {
        uint8_t args[16];
        size_t pos = pack_u64(args, sizeof args, 0, iter);
        if (pos == SIZE_MAX) break;

        uint8_t ret[256];
        size_t ret_len = 0;
        rc = p->call(h, PROXY_ENV_LIST_ALL, args, pos, ret, sizeof ret, &ret_len);
        if (rc != PROXY_OK) break;

        /* Decode: ns \0 name \0 iter(8) */
        size_t off = 0;
        const char *ns = (const char *)(ret + off);
        off += strlen(ns) + 1;
        const char *nm = (const char *)(ret + off);
        off += strlen(nm) + 1;
        iter = unpack_u64(ret + off);
        steps++;
    }
    p->unbind(h);

    if (rc != PROXY_ENOENT) {
        kprintf("  env.list_all_end: stopped with %d, expected ENOENT after %d steps\n",
                rc, steps);
        return 1;
    }
    return 0;
}

static int t_env_info(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;

    uint8_t ret[128];
    size_t ret_len = 0;
    int rc = p->call(h, PROXY_ENV_INFO, NULL, 0, ret, sizeof ret, &ret_len);
    p->unbind(h);

    if (rc != PROXY_OK) {
        kprintf("  env.info: got %d, expected OK\n", rc);
        return 1;
    }
    /* 14 u64 fields: capacity, count, used, max, timer, set, get,
     *                peek, unset, reacc, retime, respace, reexp, sweeps */
    if (ret_len != 14 * 8) {
        kprintf("  env.info: ret_len = %zu, expected %u\n", ret_len, 14 * 8);
        return 1;
    }
    uint64_t capacity = unpack_u64(ret + 0);
    if (capacity == 0) {
        kprintf("  env.info: capacity = 0 (expected > 0)\n");
        return 1;
    }
    return 0;
}

static int t_env_stale(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;
    p->unbind(h);

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    int rc = p->call(h, PROXY_ENV_GET, args, pos, NULL, 0, NULL);
    if (rc != PROXY_ESTALE) {
        kprintf("  env.stale: got %d, expected ESTALE\n", rc);
        return 1;
    }
    return 0;
}

/*
 * misc group
 *  */

static int t_misc_bind_null(const struct proxy_ops *p) {
    int rc = p->bind(NULL, NULL);
    if (rc != PROXY_EBADARG) {
        kprintf("  misc.bind_null: got %d, expected EBADARG\n", rc);
        return 1;
    }
    return 0;
}

static int t_misc_bind_missing(const struct proxy_ops *p) {
    uint32_t h = 0;
    int rc = p->bind("nonexistent", &h);
    if (rc != PROXY_ENODEV) {
        kprintf("  misc.bind_missing: got %d, expected ENODEV\n", rc);
        return 1;
    }
    return 0;
}

static int t_misc_bind_vfs_rejected(const struct proxy_ops *p) {
    uint32_t h;
    int rc = p->bind("vfs", &h);
    if (rc != PROXY_EBADCLASS) {
        kprintf("  misc.bind_vfs_rejected: got %d, expected EBADCLASS\n", rc);
        return 1;
    }
    return 0;
}

static int t_misc_bind_timer_rejected(const struct proxy_ops *p) {
    uint32_t h;
    int rc = p->bind("timer", &h);
    if (rc != PROXY_EBADCLASS) {
        kprintf("  misc.bind_timer_rejected: got %d, expected EBADCLASS\n", rc);
        return 1;
    }
    return 0;
}

static int t_misc_call_out_of_range(const struct proxy_ops *p) {
    uint8_t args[8];
    size_t len = pack_str(args, sizeof args, 0, "x");
    int rc = p->call(999, PROXY_CONSOLE_WRITE, args, len, NULL, 0, NULL);
    if (rc != PROXY_ESTALE) {
        kprintf("  misc.call_out_of_range: got %d, expected ESTALE\n", rc);
        return 1;
    }
    return 0;
}

static int t_misc_ret_len_zeroed(const struct proxy_ops *p) {
    uint32_t h;
    if (bind_dev(p, "env", &h) != PROXY_OK) return 1;

    /* Setup: write the entry so STAT has something to return.
     * Without this, stat() returns ENV_NOT_FOUND → PROXY_ENOENT, and the small-buffer path below is never reached. */
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);

    uint8_t args[128];
    size_t pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_VAL);
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }
    p->call(h, PROXY_ENV_SET, args, pos, NULL, 0, NULL);

    /* call STAT with a 4-byte ret_max. The real stat encoding is 40 bytes;
     * the call must fail with PROXY_EBADARG and zero ret_len. */
    pos = 0;
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NS);
    pos = pack_str(args, sizeof args, pos, ENV_TEST_NAME);
    if (pos == SIZE_MAX) { p->unbind(h); return 1; }

    uint8_t ret[4];
    size_t ret_len = 999;
    int rc = p->call(h, PROXY_ENV_STAT, args, pos, ret, sizeof ret, &ret_len);

    /* Teardown */
    cleanup_key(p, h, ENV_TEST_NS, ENV_TEST_NAME);
    p->unbind(h);

    if (rc != PROXY_EBADARG) {
        kprintf("  misc.ret_len_zeroed: got %d, expected EBADARG\n", rc);
        return 1;
    }
    if (ret_len != 0) {
        kprintf("  misc.ret_len_zeroed: ret_len = %zu, expected 0\n", ret_len);
        return 1;
    }
    return 0;
}

/*
 * test table + dispatch
 *  */

struct proxy_test {
    const char *name;
    int (*fn)(const struct proxy_ops *p);
};

static const struct proxy_test g_tests[] = {
    /* console */
    { "console.bind",            t_console_bind            },
    { "console.write",           t_console_write           },
    { "console.write_bad_arg",   t_console_write_bad_arg   },
    { "console.clear_bad_op",    t_console_clear_bad_op    },
    { "console.unknown_op",      t_console_unknown_op      },
    { "console.stale",           t_console_stale           },

    /* env */
    { "env.bind",                t_env_bind                },
    { "env.set_get",             t_env_set_get             },
    { "env.set_ns_gate",         t_env_set_ns_gate         },
    { "env.is_exist_yes",        t_env_is_exist_yes        },
    { "env.is_exist_no",         t_env_is_exist_no         },
    { "env.stat",                t_env_stat                },
    { "env.stat_missing",        t_env_stat_missing        },
    { "env.cross_owner_unset",   t_env_cross_owner_unset   },
    { "env.unset_own",           t_env_unset_own           },
    { "env.list_all_end",        t_env_list_all_end        },
    { "env.info",                t_env_info                },
    { "env.stale",               t_env_stale               },

    /* misc */
    { "misc.bind_null",          t_misc_bind_null          },
    { "misc.bind_missing",       t_misc_bind_missing       },
    { "misc.bind_vfs_rejected",  t_misc_bind_vfs_rejected  },
    { "misc.bind_timer_rejected", t_misc_bind_timer_rejected },
    { "misc.call_out_of_range",  t_misc_call_out_of_range  },
    { "misc.ret_len_zeroed",     t_misc_ret_len_zeroed     },

    { NULL, NULL }
};

static int cmd_proxytest_fn(const struct cmd_args *a) {
    const struct proxy_ops *p = bus_proxy();
    if (p == NULL) {
        kprintf("proxytest: proxy device not registered\n");
        return 1;
    }

    const char *filter = (a->argc >= 1) ? a->argv[0] : NULL;
    size_t filter_len = filter ? strlen(filter) : 0;

    int pass = 0, fail = 0, skipped = 0;
    for (size_t i = 0; g_tests[i].name != NULL; i++) {
        if (filter != NULL && strncmp(g_tests[i].name, filter, filter_len) != 0) {
            skipped++;
            continue;
        }
        int rc = g_tests[i].fn(p);
        if (rc == 0) {
            kprintf("PASS  %s\n", g_tests[i].name);
            pass++;
        } else {
            kprintf("FAIL  %s\n", g_tests[i].name);
            fail++;
        }
    }

    if (filter != NULL)
        kprintf("proxytest: %d pass, %d fail (%d skipped, filter=\"%s\")\n",
                pass, fail, skipped, filter);
    else
        kprintf("proxytest: %d pass, %d fail\n", pass, fail);

    return (fail == 0) ? CMD_OK : 1;
}

SYM_COMMAND(proxytest, "[filter:str]", "Proxy wire-format tester", cmd_proxytest_fn);

static int proxytest_ext_init(void) {
    if (bus_proxy() == NULL)
        kprintf("proxytest: proxy not yet registered (tests will report)\n");
    return 0;
}

SYM_EXTENSION(proxytest, proxytest_ext_init, EXT_PRIO_APPLET);

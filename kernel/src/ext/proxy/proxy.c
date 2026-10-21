#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <lib/string.h>
#include <core/dev/env.h>
#include <core/dev/console.h>
#include <core/dev/timer.h>
#include <core/dev/vfs.h>
#include <core/bus.h>
#include <core/klog.h>
#include <core/mask.h>
#include <core/ext.h>
#include <ext/proxy/proxy.h>

#define PROXY_MAX 32

struct proxy_slot {
    bool     live;
    uint16_t class;
    const struct bus_device *dev;   /* stable pointer into bus_table */
};

static struct proxy_slot g_slots[PROXY_MAX];

/*  helpers  */

static const char *arg_next(const uint8_t *args, size_t args_len,
                            size_t *pos) {
    if (*pos >= args_len) return NULL;
    const char *s = (const char *)(args + *pos);
    size_t remain = args_len - *pos;
    size_t n = 0;
    while (n < remain && s[n] != '\0') n++;
    if (n == remain) return NULL;      /* no NUL terminator */
    *pos += n + 1;
    return s;
}

static bool take_u64(const uint8_t *args, size_t args_len,
                     size_t *pos, uint64_t *out) {
    if (*pos + 8 > args_len) return false;
    memcpy(out, args + *pos, 8);
    *pos += 8;
    return true;
}

static bool take_u32(const uint8_t *args, size_t args_len,
                     size_t *pos, uint32_t *out) {
    if (*pos + 4 > args_len) return false;
    memcpy(out, args + *pos, 4);
    *pos += 4;
    return true;
}

static bool put_u64(uint8_t *ret, size_t ret_max, size_t *pos, uint64_t v) {
    if (*pos + 8 > ret_max) return false;
    memcpy(ret + *pos, &v, 8);
    *pos += 8;
    return true;
}

static bool put_u32(uint8_t *ret, size_t ret_max, size_t *pos, uint32_t v) {
    if (*pos + 4 > ret_max) return false;
    memcpy(ret + *pos, &v, 4);
    *pos += 4;
    return true;
}

static bool put_str(uint8_t *ret, size_t ret_max, size_t *pos, const char *s) {
    size_t n = strlen(s) + 1;
    if (*pos + n > ret_max) return false;
    memcpy(ret + *pos, s, n);
    *pos += n;
    return true;
}

/*  per-class dispatch  */

static int console_call(struct proxy_slot *s, uint16_t op,
                        const uint8_t *args, size_t args_len,
                        uint8_t *ret, size_t ret_max, size_t *ret_len) {
    const struct console_ops *ops = s->dev->ops;
    (void)ret; (void)ret_max;

    switch (op) {
    case PROXY_CONSOLE_WRITE: {
        size_t pos = 0;
        const char *str = arg_next(args, args_len, &pos);
        if (str == NULL) return PROXY_EBADARG;
        /* console_ops->write takes (const char *); adapt if signature differs */
        size_t len = strlen(str);
        ops->write(str, len);
        if (ret_len) *ret_len = 0;
        return PROXY_OK;
    }
    case PROXY_CONSOLE_CLEAR:
        //ops->clear();
        if (ret_len) *ret_len = 0;
        return PROXY_OK;
    default:
        return PROXY_EBADOP;
    }
}

static int env_call(struct proxy_slot *s, uint16_t op,
                    const uint8_t *args, size_t args_len,
                    uint8_t *ret, size_t ret_max, size_t *ret_len) {
    const struct env_ops *ops = s->dev->ops;
    size_t out_pos = 0;

    switch (op) {
    case PROXY_ENV_GET:
    case PROXY_ENV_PEEK: {
        size_t pos = 0;
        const char *ns   = arg_next(args, args_len, &pos);
        const char *name = arg_next(args, args_len, &pos);
        if (!ns || !name) return PROXY_EBADARG;
        size_t got = 0;
        int rc = (op == PROXY_ENV_GET)
               ? ops->get(ns, name, (char *)ret, ret_max, &got)
               : ops->peek(ns, name, (char *)ret, ret_max, &got);
        if (rc == ENV_BUFFER_TOO_SMALL) return PROXY_EBADARG;
        if (rc != ENV_OK) return PROXY_EINTERNAL;
        if (ret_len) *ret_len = got;
        return PROXY_OK;
    }
    case PROXY_ENV_SET: {
        size_t pos = 0;
        const char *ns    = arg_next(args, args_len, &pos);
        const char *name  = arg_next(args, args_len, &pos);
        const char *value = arg_next(args, args_len, &pos);
        if (!ns || !name || !value) return PROXY_EBADARG;
        /* Proxy owns mask construction: script cannot forge owner/type/flags. */
        uint32_t mask = env_mask_build((uint16_t)BUS_PROXY, ENV_TYPE_STRING, 0, 0);
        int rc = ops->set(ns, name, value, 0, 0, 0, mask);
        if (rc != ENV_OK) return PROXY_EINTERNAL;
        if (ret_len) *ret_len = 0;
        return PROXY_OK;
    }
    case PROXY_ENV_UNSET: {
        size_t pos = 0;
        const char *ns   = arg_next(args, args_len, &pos);
        const char *name = arg_next(args, args_len, &pos);
        if (!ns || !name) return PROXY_EBADARG;
        /* Ownership gate: only entries the interpreter owns may be unset. */
        struct env_stat st;
        if (ops->stat(ns, name, &st) != ENV_OK) return PROXY_EINTERNAL;
        if (env_mask_owner(st.mask) != BUS_PROXY)
            return PROXY_EBADOP;
        int rc = ops->unset(ns, name);
        if (rc != ENV_OK) return PROXY_EINTERNAL;
        if (ret_len) *ret_len = 0;
        return PROXY_OK;
    }
    case PROXY_ENV_IS_EXIST: {
        size_t pos = 0;
        const char *ns   = arg_next(args, args_len, &pos);
        const char *name = arg_next(args, args_len, &pos);
        if (!ns || !name) return PROXY_EBADARG;
        int rc = ops->is_exist(ns, name);
        if (rc != ENV_OK && rc != ENV_NOT_FOUND) return PROXY_EINTERNAL;
        if (rc == ENV_NOT_FOUND) return PROXY_EINTERNAL;   /* or PROXY_ENOENT later */
        if (ret_len) *ret_len = 0;
        return PROXY_OK;
    }
    case PROXY_ENV_STAT: {
        size_t pos = 0;
        const char *ns   = arg_next(args, args_len, &pos);
        const char *name = arg_next(args, args_len, &pos);
        if (!ns || !name) return PROXY_EBADARG;
        struct env_stat st;
        if (ops->stat(ns, name, &st) != ENV_OK) return PROXY_EINTERNAL;
        if (!put_u32(ret, ret_max, &out_pos, st.value_len))       return PROXY_EBADARG;
        if (!put_u32(ret, ret_max, &out_pos, st.access_remaining))return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, st.has_ttl ? 1 : 0)) return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, st.ttl_remaining))   return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, st.last_access))     return PROXY_EBADARG;
        if (!put_u32(ret, ret_max, &out_pos, st.importance))      return PROXY_EBADARG;
        if (!put_u32(ret, ret_max, &out_pos, st.mask))            return PROXY_EBADARG;
        if (ret_len) *ret_len = out_pos;
        return PROXY_OK;
    }
    case PROXY_ENV_LIST: {
        size_t pos = 0;
        const char *ns = arg_next(args, args_len, &pos);
        uint64_t it = 0;
        if (!ns || !take_u64(args, args_len, &pos, &it)) return PROXY_EBADARG;
        size_t iter = (size_t)it;
        char namebuf[ENV_NAME_MAX + 1];
        int rc = ops->list(ns, &iter, namebuf, sizeof namebuf);
        if (rc != ENV_OK && rc != ENV_NOT_FOUND) return PROXY_EINTERNAL;
        if (rc == ENV_NOT_FOUND) return PROXY_EINTERNAL;
        if (!put_str(ret, ret_max, &out_pos, namebuf))       return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)iter))return PROXY_EBADARG;
        if (ret_len) *ret_len = out_pos;
        return PROXY_OK;
    }
    case PROXY_ENV_LIST_ALL: {
        size_t pos = 0;
        uint64_t it = 0;
        if (!take_u64(args, args_len, &pos, &it)) return PROXY_EBADARG;
        size_t iter = (size_t)it;
        char nsbuf[ENV_NAME_MAX + 1], namebuf[ENV_NAME_MAX + 1];
        int rc = ops->list_all(&iter, nsbuf, sizeof nsbuf, namebuf, sizeof namebuf);
        if (rc != ENV_OK) return PROXY_EINTERNAL;
        if (!put_str(ret, ret_max, &out_pos, nsbuf))         return PROXY_EBADARG;
        if (!put_str(ret, ret_max, &out_pos, namebuf))       return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)iter))return PROXY_EBADARG;
        if (ret_len) *ret_len = out_pos;
        return PROXY_OK;
    }
    case PROXY_ENV_LIST_MASK: {
        size_t pos = 0;
        uint32_t mask = 0;
        uint64_t it = 0;
        if (!take_u32(args, args_len, &pos, &mask)) return PROXY_EBADARG;
        if (!take_u64(args, args_len, &pos, &it))   return PROXY_EBADARG;
        size_t iter = (size_t)it;
        char nsbuf[ENV_NAME_MAX + 1], namebuf[ENV_NAME_MAX + 1];
        int rc = ops->list_by_mask(mask, &iter, nsbuf, sizeof nsbuf, namebuf, sizeof namebuf);
        if (rc != ENV_OK) return PROXY_EINTERNAL;
        if (!put_str(ret, ret_max, &out_pos, nsbuf))         return PROXY_EBADARG;
        if (!put_str(ret, ret_max, &out_pos, namebuf))       return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)iter))return PROXY_EBADARG;
        if (ret_len) *ret_len = out_pos;
        return PROXY_OK;
    }
    case PROXY_ENV_COUNT_MASK: {
        size_t pos = 0;
        uint32_t mask = 0;
        if (!take_u32(args, args_len, &pos, &mask)) return PROXY_EBADARG;
        size_t n = 0;
        if (ops->count_by_mask(mask, &n) != ENV_OK) return PROXY_EINTERNAL;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)n)) return PROXY_EBADARG;
        if (ret_len) *ret_len = out_pos;
        return PROXY_OK;
    }
    case PROXY_ENV_INFO: {
        struct env_info inf;
        if (ops->info(&inf) != ENV_OK) return PROXY_EINTERNAL;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)inf.capacity))    return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)inf.count))       return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)inf.used_bytes))  return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, (uint64_t)inf.max_bytes))   return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.timer_attached ? 1 : 0))return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_set))                 return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_get))                 return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_peek))                return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_unset))               return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_reclaim_access))      return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_reclaim_time))        return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_reclaim_space))       return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_reclaim_explicit))    return PROXY_EBADARG;
        if (!put_u64(ret, ret_max, &out_pos, inf.n_sweeps))              return PROXY_EBADARG;
        if (ret_len) *ret_len = out_pos;
        return PROXY_OK;
    }
    default:
        return PROXY_EBADOP;
    }
}

/* vfs_call / timer_call: stubs until the ops structs are catalogued. */

static int vfs_call(struct proxy_slot *s, uint16_t op,
                    const uint8_t *args, size_t args_len,
                    uint8_t *ret, size_t ret_max, size_t *ret_len) {
    (void)s; (void)op; (void)args; (void)args_len;
    (void)ret; (void)ret_max; (void)ret_len;
    return PROXY_EBADOP;
}

static int timer_call(struct proxy_slot *s, uint16_t op,
                      const uint8_t *args, size_t args_len,
                      uint8_t *ret, size_t ret_max, size_t *ret_len) {
    (void)s; (void)op; (void)args; (void)args_len;
    (void)ret; (void)ret_max; (void)ret_len;
    return PROXY_EBADOP;
}

/*  proxy ops  */

static int proxy_bind(const char *name, uint32_t *handle_out) {
    if (name == NULL || handle_out == NULL) return PROXY_EBADARG;

    const struct bus_device *d = bus_find(name);
    if (d == NULL) return PROXY_ENODEV;

    /* Only these classes are script-visible. Anything else is denied. */
    switch (d->class) {
    case BUS_CLASS_CONSOLE:
    case BUS_CLASS_ENV:
    case BUS_CLASS_VFS:
    case BUS_CLASS_TIMER:
        break;
    default:
        return PROXY_EBADCLASS;
    }

    for (uint32_t i = 0; i < PROXY_MAX; i++) {
        if (!g_slots[i].live) {
            g_slots[i].live  = true;
            g_slots[i].class = (uint16_t)d->class;
            g_slots[i].dev   = d;
            *handle_out = i;
            return PROXY_OK;
        }
    }
    return PROXY_EFULL;
}

static int proxy_unbind(uint32_t handle) {
    if (handle >= PROXY_MAX) return PROXY_ESTALE;
    if (!g_slots[handle].live) return PROXY_ESTALE;
    g_slots[handle].live = false;
    g_slots[handle].dev  = NULL;
    g_slots[handle].class = 0;
    return PROXY_OK;
}

static int proxy_call(uint32_t handle, uint16_t op,
                      const uint8_t *args, size_t args_len,
                      uint8_t *ret, size_t ret_max, size_t *ret_len) {
    if (handle >= PROXY_MAX) return PROXY_ESTALE;
    struct proxy_slot *s = &g_slots[handle];
    if (!s->live || s->dev == NULL) return PROXY_ESTALE;

    /* Staleness: the device must still be the one the bus resolves. */
    if (bus_find(s->dev->name) != s->dev) {
        s->live = false;
        s->dev  = NULL;
        return PROXY_ESTALE;
    }

    switch (s->class) {
    case BUS_CLASS_CONSOLE: return console_call(s, op, args, args_len, ret, ret_max, ret_len);
    case BUS_CLASS_ENV:     return env_call    (s, op, args, args_len, ret, ret_max, ret_len);
    case BUS_CLASS_VFS:     return vfs_call    (s, op, args, args_len, ret, ret_max, ret_len);
    case BUS_CLASS_TIMER:   return timer_call  (s, op, args, args_len, ret, ret_max, ret_len);
    default:                return PROXY_EBADCLASS;
    }
}

static const struct proxy_ops g_proxy_ops = {
    .bind   = proxy_bind,
    .unbind = proxy_unbind,
    .call   = proxy_call,
};

const struct proxy_ops *bus_proxy(void) {
    const struct bus_device *d = bus_find_class(BUS_CLASS_PROXY, 0);
    return (d != NULL) ? (const struct proxy_ops *)d->ops : NULL;
}

/*  init  */

int proxy_init(void) {
    for (uint32_t i = 0; i < PROXY_MAX; i++) {
        g_slots[i].live  = false;
        g_slots[i].dev   = NULL;
        g_slots[i].class = 0;
    }

    struct bus_device dev = {
        .name         = "proxy",
        .class        = BUS_CLASS_PROXY,
        .version      = 1,
        .ops          = &g_proxy_ops,
        .private_data = NULL,
        .owner_id     = BUS_PROXY,
    };
    if (bus_register(&dev) != 0)
        return -1;
    bus_test("proxy");
    return 0;
}

SYM_EXTENSION(proxy, proxy_init, EXT_PRIO_SERVICE);

#ifndef EXT_PROXY_PROXY_H
#define EXT_PROXY_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Proxy: the single mediation boundary between the interpreter and the bus.
 *
 * The interpreter NEVER holds a raw ops pointer. It holds a uint32_t handle
 * that only this device can mint, validate, or invalidate. Every capability
 * the script can invoke goes through proxy_call(), which:
 *   1. re-validates the handle against the bus (staleness),
 *   2. checks the op is whitelisted for the handle's class,
 *   3. validates the arg payload,
 *   4. casts dev->ops to the concrete ops struct and invokes.
 *
 * Handles are small integers, opaque to callers.
 * A handle whose backing device is gone returns PROXY_ESTALE — never a dangling pointer.
 *
 * The catalogue of exposed ops per class lives in proxy.c, one switch per class.
 * What is not in that switch is not reachable from a script.
 */

enum proxy_result {
    PROXY_OK = 0,
    PROXY_ESTALE,       /* handle unknown, unbound, or device gone */
    PROXY_EBADCLASS,    /* handle's class doesn't match what the op expects */
    PROXY_EBADOP,       /* op not whitelisted for this class */
    PROXY_EBADARG,      /* argument payload malformed or out of range */
    PROXY_ENODEV,       /* bind: no such device */
    PROXY_EFULL,        /* bind: handle table full */
    PROXY_EINTERNAL,
};

/* Op selectors are per-class.
 * The interpreter's native table maps "console.write" -> (handle, PROXY_CONSOLE_WRITE, arg spec).
 * Selectors are stable numbers; adding one is an append, never a renumber. */

enum proxy_console_op {
    PROXY_CONSOLE_WRITE = 1,
    PROXY_CONSOLE_CLEAR = 2,
};

enum proxy_env_op {
    PROXY_ENV_GET         = 1,   /* args: ns \0 name; ret: value bytes */
    PROXY_ENV_PEEK        = 2,   /* same, does not consume budget */
    PROXY_ENV_SET         = 3,   /* args: ns \0 name \0 value; mask built by proxy */
    PROXY_ENV_UNSET       = 4,   /* args: ns \0 name; proxy checks ownership */
    PROXY_ENV_IS_EXIST    = 5,   /* args: ns \0 name; ret: empty on ENV_OK */
    PROXY_ENV_STAT        = 6,   /* args: ns \0 name; ret: encoded stat */
    PROXY_ENV_LIST        = 7,   /* args: ns \0 iter(8 bytes); ret: name \0 iter */
    PROXY_ENV_LIST_ALL    = 8,   /* args: iter; ret: ns \0 name \0 iter */
    PROXY_ENV_LIST_MASK   = 9,   /* args: mask(4) iter(8); ret: ns \0 name \0 iter */
    PROXY_ENV_COUNT_MASK  = 10,  /* args: mask(4); ret: count(8) */
    PROXY_ENV_INFO        = 11,  /* args: none; ret: encoded info */
    /* no drop_namespace */
};

struct proxy_ops {
    /* Resolve a device by name. Class is validated against a known set.
     * `handle_out` receives an opaque handle. */
    int (*bind)(const char *name, uint32_t *handle_out);

    /* Release a handle. Any subsequent call returns PROXY_ESTALE. */
    int (*unbind)(uint32_t handle);

    /* Invoke a whitelisted op on a bound handle.
     *   args/args_len   -> op-specific payload (see enum above)
     *   ret/ret_max     -> output buffer
     *   ret_len         -> actual bytes written (may be NULL if unused)
     * Returns a PROXY_* code. */
    int (*call)(uint32_t handle, uint16_t op,
                const uint8_t *args, size_t args_len,
                uint8_t *ret, size_t ret_max, size_t *ret_len);
};

const struct proxy_ops *bus_proxy(void);

/* Extension entry point. */
int proxy_init(void);

#endif /* EXT_PROXY_PROXY_H */

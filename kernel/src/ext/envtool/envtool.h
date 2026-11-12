#ifndef EXT_ENVTOOL_H
#define EXT_ENVTOOL_H

#include <stddef.h>
#include <stdint.h>

/* envtool: inspection utilities for the environment store.
 *
 * The extension registers one command, `envtool`, with subcommands:
 *
 *   envtool [inspect]      interactive TUI viewer (default)
 *   envtool stats          device snapshot and per-flag counts
 *
 * Stress / clear tooling lives in a separate extension (`stressor`); envtool
 * is read-only by design and never mutates the store.
 *
 * Context: thread context only. Do not call from an ISR.
 *
 * Note on SECRET entries: `inspect` uses env->list_all(), which honors the
 * SECRET visibility contract — entries with ENV_FLAG_SECRET do not appear.
 * A future `inspect --secret` flag would query via
 * env->list_match(ENV_Q_FLAG(ENV_FLAG_SECRET), ...) instead; today the
 * viewer cannot see them, by design.
 */

/* Print a device snapshot and per-flag counts to the console.
 * Read-only; does not modify the store. Returns 0 on success. */
int envtool_stats(void);

#endif /* EXT_ENVTOOL_H */

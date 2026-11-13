#ifndef CORE_SHELL_H
#define CORE_SHELL_H

/* codebase: 0.1.2 */

/* Interactive command loop.
 *
 * The shell is now a state machine ticked by the kernel's main loop,
 * rather than an infinite blocking loop itself. This allows background
 * timers and idle tasks to run even when the user is not typing.
 *
 * Call shell_tick() from kmain's main loop.
 * Call shell_run() only if you want the legacy blocking behavior (e.g.,
 * for a debugger prompt that absolutely must not be interrupted by timers).
 */

/* Initialize the shell state (clears buffers, resets prompt). */
void shell_init(void);

/* Non-blocking tick. Pass a character if one was read, or 0 if none.
 * Returns true if the shell is actively waiting for a character,
 * false if it just executed a command and the loop should maybe redraw. */
void shell_tick(char c);

/* Legacy blocking loop. Never returns. */
__attribute__((noreturn))
void shell_run(void);

#endif

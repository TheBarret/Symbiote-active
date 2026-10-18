#ifndef CORE_PANIC_H
#define CORE_PANIC_H

/* Stop everything and say exactly what went wrong and where. */
__attribute__((noreturn, format(printf, 3, 4)))
void panic_at(const char *file, int line, const char *fmt, ...);

/* A CPU exception (or unexpected vector) with the full saved state:
 * decoded error code, every register, a backtrace starting at the faulting instruction. */
struct isr_frame;
__attribute__((noreturn))
void panic_exception(const struct isr_frame *f);

#define PANIC(...)  panic_at(__FILE__, __LINE__, __VA_ARGS__)

#define ASSERT(cond) \
    ((cond) ? (void)0 : panic_at(__FILE__, __LINE__, "assertion failed: %s", #cond))

#endif

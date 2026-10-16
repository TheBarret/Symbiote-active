# Symbiote-active

<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

<img width="1290" height="850" alt="0.1.1" src="https://github.com/user-attachments/assets/0e444b29-62c9-4ae6-91dd-2dd370c977dd" />  

*Symbiote kernel 0.1.1, active development*  

Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   

## Changelog

**0.1.0** - Initial codebase & Primary foundation  
- `lib/mem.h`, `lib/mem.c`: added `memchr`, `explicit_bzero`, and named align/bit helpers (`is_power_of_two`, `align_up`, `align_down`, `ptr_align_up`, `ptr_align_down`, `bit_test`, `bit_set`, `bit_clear`).
- `lib/string.h`, `lib/string.c`: added `strrchr`, `strcat`, `strncat`, `strspn`, `strcspn`, `strpbrk`, `strstr`, `strtoull`, `strtoll`, and the strict wrapper `kstrtoull`.
- `lib/string.c`: rewrote `strtoll`'s overflow check as two explicit branches.
- `core/cmd.c`: `parse_num` now calls `kstrtoull`; `parse_spec` uses `strspn`; self-check now rejects whitespace in command names.
- `core/kprintf.h`, `core/kprintf.c`: hardened rewrite. Exposed `kemit_fn`/`kvformat`, added precision (`.N`/`.*`), added `%o` and `%b`, made `h` truncate to 16 bits, sized the digit buffer for base 2, replaced the long parameter list with `struct field`.
- `core/heap.c`: `struct chunk` records its own size; `heapcheck` uses it instead of assuming `HEAP_CHUNK_SIZE`.
- `core/heap.c`: `kmalloc` and `krealloc` now reject sizes that would overflow the alignment and header arithmetic.
- `core/pmm.c`: bitmap placement in Pass 2 now aligns the candidate base before checking size against the entry's end.
- `core/klog.c`: fixed a missing closing brace in `klog_init` that collapsed the function body. Added timestamp prefix: `rdtsc`-based delta before the PIT is up, `[+ms.us]` after, with 1 GHz fallback.
- `core/cpu.h`, `core/cpu.c`: added `rdtsc()` and `cpu_tsc_hz()` (CPUID 0x15, family/model table for the `ECX == 0` case).
- `core/vfs.h`, `core/vfs.c`: new VFS layer. Node model, path normalization with `.` and `..`, `vfs_open`/`close`/`read`/`write`/`lseek`, `vfs_mkdir`/`unlink`/`stat`/`readdir`. Single global spinlock with IRQ-save semantics. `vfs_open` allocates the handle before linking the node. `vfs_lseek` does all offset math in `uint64_t` with explicit sign handling.
- `core/tui.h`, `core/tui.c`: small TUI core. Two cell grids, rect drawing (`tui_put`/`tui_text`/`tui_fill`/`tui_box`), differential flush, `struct tui_view` with draw/key callbacks.
- `core/keyboard.c`, `core/keyboard.h`: added `kbd_wait()`, which blocks on a decodable key event via `cpu_sti_hlt`.
- `src/ext/fs/`: VFS shell commands `ls`, `cat`, `mkdir`, `rm`, `write`, plus an `fs` extension init.
- `src/ext/explorer/explorer.c`: single-panel VFS browser. Arrow keys move, Enter descends, Backspace ascends, `q` quits. Scrolling list, highlighted selection, framed layout.
- `core/tui.h`: added a screen-ownership invariant comment. `tui_begin`/`tui_end` bracket a session; between them the TUI owns the screen, before and after the console does, with default SGR and a visible cursor.
- `core/tui.h`: documented that `TUI_ATTR(fg, bg)` backgrounds are honoured by `tui_flush`; previously the byte was accepted and silently dropped.
- `core/tui.h`, `core/tui.c`: added `tui_run_view`, which brackets `tui_run` with `tui_begin`/`tui_end`. Prevents a view that returns early, or a draw callback that panics, from stranding the screen in TUI mode with the cursor hidden.
- `core/tui.c`: `tui_begin` now emits `\x1b[0m` before clearing. A prior command's SGR no longer leaks into the TUI's first frame.
- `core/tui.c`: `tui_end` now resets SGR, clears the screen, and homes the cursor, in addition to showing it. The previous version relied on `tui_begin`'s clear and used a trailing `\n` as a stand-in for home.
- `core/tui.c`: `tui_flush` now emits both nibbles of the attribute byte. Backgrounds were advertised by the macro and ignored by the emitter.
- `core/tui.c`: `tui_flush` drains to `console_write` in chunks instead of accumulating into a fixed 8 KiB buffer. The old buffer filled silently on full-screen changes, dropping cells and leaving the front grid out of sync with the back grid.
- `core/tui.c`: `tui_flush` now advances `last_y` alongside `last_x` after each character write. `last_y` was previously set only inside the cursor-move branch, which happened to work but was not an invariant.
- `core/tui.c`: moved the SGR tables to file scope and deleted the dead `emit_attr` helper.
- `core/tui.c`: replaced hand-counted escape-sequence lengths with `sizeof - 1` over file-scope string constants.
- `core/pic.h`: documented that `pic_eoi` must not be called after `pic_irq_is_spurious` returns true; that function already sent the correct (or no) EOI, and a second one desyncs the in-service register.
- `core/pic.c`: `pic_eoi` now bounds-checks `irq`, matching `pic_mask` and `pic_unmask`. An out-of-range call previously sent an EOI with no corresponding interrupt.
- `core/pic.c`: `pic_mask` and `pic_unmask` now bracket the read-modify-write of `irq_mask_bits` and the two `write_masks` `outb`s in a `cpu_irq_save`/`cpu_irq_restore` region. Removes a lost-update race with nested or future SMP callers.
- `core/pic.c`: `lapic_virtual_wire` now logs when the LAPIC MMIO map fails. Silent failure on the boot path meant IRQs never arrived and the only clue was the absence of activity.
- `core/cpu.h`: port I/O (`outb`, `inb`, `inw`, `outl`, `inl`) now carries a `"memory"` clobber. Port accesses can have memory side effects and must not be reordered relative to memory.
- `core/cpu.h`: `cpu_hlt` now carries a `"memory"` clobber. It is a synchronisation point; code after the halt may depend on data the wakeup interrupt wrote.
- `core/cpu.h`: `read_cr2`, `read_cr3`, `write_cr3`, and `invlpg` now carry a `"memory"` clobber. A CR3 write remaps the whole address space; an `invlpg` invalidates a TLB entry. Stores must not hoist across either.
- `core/cpu.h`: added `outw` for symmetry with `inw`.
- `core/cpu.h`: moved `outl` and `inl` up into the Port I/O section. They were filed under "PCI handlers" and read as PCI-specific when they are not.
- `core/tui`: expanding TUI logic, fixing some minor issues and performance improvement.
- `core/config.h`: created, begun to refactor global kernel settings, by analyzing all `core/*` modules, taking abstract parameters, mostly types of; sizes, shapes, maximums and minimums.
- `core/version.h`: added ref: `config.h`, all core codebase sources should `#include <version.h>`, it now comes with `config.h` included, resolving module parameter reference overlays.
- `core/serial.h`: marked for a new design, current logic is not functional on `QEMU -serial pty -display none -monitor none` / `picocom` host-terminal mode, only output, no input, `shell.c` is using PS/2 exclusively at this moment.
- `core/host.c`, `core/host.h`, `src/ext/configure/*`: Probe extension and driver removed (out of favor).
- `core/bus.c` + `core/bus.h`: the registry, static array, `BUS_MAX_DEVICES 32`, no heap, no locks. `bus_init` → `bus_register` → `bus_find` / `bus_find_class` / `bus_count` / `bus_at`.
- `timer` extended to a real device: `timer_after` / `timer_cancel` / `timer_drain`, ISR sets ready, drain runs decoupled, callbacks in thread context. Wired into `kbd_getchar`'s idle path so scheduling works while the shell is blocked.
- Built-in devices on the bus, one per class: `console` (1), `timer` (2), `vfs` (3). Typed finders `bus_console()`, `bus_timer()`, `bus_vfs()` all working, ops tables dispatching to the real functions.

**0.1.1** - Environment codebase  

<img width="1004" height="547" alt="Environment" src="https://github.com/user-attachments/assets/4343e103-5507-4315-a907-386609a3aab7" />  


- `core/timer.h`, `core/timer.c`:  
  added helpers for milliseconds to timer ticks conversions (`ms_to_ticks_floor`, `ms_to_ticks_ceil`,`ms_to_ticks_round`).
  Rounding policy is now explicit at every call site, and the conversions are overflow-safe.
  `timer_sleep_ms` and `timer_after` were updated to use the ceil variant, which is what both have always implicitly wanted.

- `core/env.c`, `core/env.h`, `ext/env/env.c`:  
  added the `environment` device and its shell toolkit. A bounded, fixed-capacity store of `(namespace, name) -> value` entries,
  with per-entry access budgets, time budgets, importance levels, and free-form classification masks.
  Reclamation follows OOM-model principles: entries are reclaimed synchronously when their access budget is exhausted,
  asynchronously by a timer-driven sweeper when their time budget passes, and under space pressure by an importance-ordered eviction policy.
  The device is registered on the bus as `BUS_CLASS_ENV` and reached through `bus_env()`.

- `ext/envtool/`: added the `envtool` using the `TUI` engine to browse and inspect the `environment` variable store.  
- `ext/btool/`: added the `btool` extension app for inspecting the bus registry.
  
## Environment Layers

The `environment` module is basically a storage device shaped as a driver on the bus registry,  
it stores and shares variables with a lifecycle:  

    A bounded, fixed-capacity key-value store where each entry is a string addressed by a `(namespace, name)` pair,  
    or with the `MASK` field for arbitrary bitmask matching, the table is statically bounded, cannot grow without limit.  

<img width="736" height="223" alt="env" src="https://github.com/user-attachments/assets/1be556f2-ad66-405b-9e16-07c058339d41" />  


Entries carry explicit lifetimes:  

    An access budget that is consumed by reads, a time budget that is enforced by a periodic sweeper,  
    and an importance value that orders which entries are sacrificed when space runs short.  
    All three mechanisms converge on a single reclamation path,  
    so the rules for when an entry dies are stated once and applied uniformly.  

<img width="750" height="455" alt="envtool" src="https://github.com/user-attachments/assets/bcff0efc-0d77-46ae-b09a-bd3af6b9e34e" />  

*using `envtool` extension to inspect all variables*  
 
It serves three platforms, in order of appearance:  

**The kernel and extensions scope**  

which use it as a place to record runtime state  data that would otherwise need to be threaded through call chains as function arguments.  
The `shell.cwd`, `shell.last_exit`, and comparable keys are the first instance of this pattern;  
future devices (System, in particular) will register their own namespaces and expose their values the same way.  

**Interpreters and Script engines**  

which reach the device through interpreters or script engines and use it to write/read/store between script invocations.  
A script that runs, deposits a result under a well-known key, and exits leaves a durable artifact for the next script to pick up.  
The access budget model makes the one-shot handoff, write once, read once, gone, the natural default,  
rather than a pattern that has to be enforced by convention.  

**The shell**  

which uses the device's shell command as a live inspection tool.  
The `env stat` view, the `env list` view and the device-level counters in `env stats`,  
together turn the environment from an opaque store into a piece of kernel state that can be examined,  
while the system is running without adding any diagnostic code to the device itself.  

---

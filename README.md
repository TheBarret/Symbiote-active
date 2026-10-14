# Symbiote-active

<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

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

**0.1.1**  
- Added: `environment` extended to a virtual storage device, volatile session variables, ring buffer, per node can assign auto-purge options; `access`,`ttl` or `importance` (OOM-model), that determine life cycle of data.

  <img width="659" height="544" alt="image" src="https://github.com/user-attachments/assets/8e17c1cb-d392-498a-903a-3d94a6b28cfd" />  

- Added: `btool` extension app to inspect device bus, read out of raw data and pointers.

  <img width="767" height="447" alt="image" src="https://github.com/user-attachments/assets/b634278e-a12c-428f-8bc0-f067f9ceb7df" />  


## Assets

<img width="572" height="1024" alt="diagram" src="https://github.com/user-attachments/assets/8436ab96-6393-4492-9bc3-3ea02094e73f" />  



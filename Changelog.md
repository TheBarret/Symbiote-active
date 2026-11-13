# Active changes & log 0.1.2 (LC-3, DEC)

- `core/bus.c` publishes the full catalog of the bus registry: `kernel.dev.<name>.*`.
- `ext/fbcon/fbcon.c` publishes console `column, row, width, height`: `kernel.dev.console.*`.
- `core/tui.c` gained `tui_fetch_resolution()` and `CP437` support.
- `ext/env/env.c`, `ext/envtool/`: the `env` command-line tool, plus `envtool` inspection and stress testing, with `CP437` support.
- `ext/fbcon/fbcon.c`: the terminal now uses the kernel's `heap->kmalloc` pool.
- `ext/memory/md.c`: memory dump tool, safely scans memory in both directions without faulting.
  
- `core/dev/keyboard.c` publishes keystroke vectors `keystrokes, chars, dropped, scancode, ticks, modifiers, last_char`: `kernel.dev.keyboard.*`.
- `core/dev/timer.c` publishes delta vectors `ticks, uptime_ms, hz`: `kernel.dev.timer.*`.
- `core/dev/env.c`: fixed a memory leak in `env_set`'s overwrite-flag logic.
- `ext/script/decimal.c`, `ext/script/vm16.c`: software-based decimals, and started with a LC-3-style 16-bit native VM extension.

- `core/dev/keyboard.c`, `dev/keyboard.h`: patched with new `bus_ops` initializer table, exposing `locks` and `drops` capabilities.
- `ext/script/decimal.c`, `ext/script/decimal.h`: patched and cleaned up.
- `ext/script/vm16.c`, `ext/script/vm16.h`: continuation of first LC-3 functions, has its own bus address and `bus_ops` capabilities.

- `dev/keyboard.c`, `main.c`: fixed incorrect load order of the keyboard readiness (`bus_settle()` manifest missed it).

- `ext/script/decimal.c`, `ext/script/test.c`, `ext/script/vm16.c`: added `DEC_CMP` operator, `vm16.c` can process basic `decimals`. added testers.

- `ext/fbcon/fbcon.c` refactored, `Init → register → publish → brand`.
- `ext/fbcon/fbcon_brand.c` added, draws top-right branding using `fbcon` (asset: `ext/assets/logo.h`).
- `ext/fbcon/fbcon_manifest.c` added. publishes `fbcon` dimension fields on `env` store.

- `ext/script/decimal.c`, `ext/script/test.c`, `ext/script/vm16.c`: refactored, patched `DEC_CMP` with `FAULT` logic.

- `core/version.c`: bumped up to `0.1.2`, major changes: `LC-3` and `Decimal` features, added testers;
	* `dec_selftest()`
    * `vm16_decimal_integration_test()`
	* `vm16_decimal_fault_test()`
 	* `vm16_decimal_setget_test()`

- `core/dev/env.c` added device-level aggregate bitmask (`active_mask_or`) within struct `env_device` to support `O(1)` short-circuit evaluation  
   for mask-based queries (`env_list_by_mask` and `env_count_by_mask`), integrated automatic recalculation hooks into `env_set()` and `env_reclaim()`
   to maintain bitfield synchronization without iterative overhead during read paths.

- `core/dev/env.c`: **`READONLY` and `PINNED` are now actually enforced** on overwrite. The gate previously tested the shifted `env_mask_flags()` value against the unshifted `ENV_FLAG_*` constants and could never fire.
- `core/dev/env.c`: aggregate mask is now an **exact index** (`bit_cnt`, `owner_cnt`, `type_cnt`, `active_mask_or`) maintained in `O(1)` on every set and reclaim. The `mask_dirty` flag and the lazy recompute scan are removed.
- `core/dev/env.h`: new **care/want queries**, `list_match` / `count_match`, plus `ENV_Q_OWNER` / `ENV_Q_TYPE` / `ENV_Q_FLAG` in `mask.h`. `OWNER` and `TYPE` are numbers, so the old subset-style `*_by_mask` calls misreported them (owner 1 matched owners 3, 5, 7). The legacy calls remain, documented as flag/private-bit only. New ops are appended to `env_ops`, so existing consumers are unaffected.
- `core/dev/env.c`: **`set()` is all-or-nothing enforced** Strings are staged first, then eligible space is proven before anything is evicted. A failed write (`ENV_NO_SPACE`, `ENV_READONLY`, heap exhaustion) no longer evicts other entries.
- `core/dev/env.c`: eviction respects the incoming write's importance (only entries with importance ≤ the write's are eligible), and `CACHEABLE` entries are evicted before non-cacheable ones of equal importance.
- `core/dev/env.c`: `SECRET` is more strongly honored: hidden from `list`, `list_all` and from queries whose `care` omits the bit.
- `core/dev/env.c`: an expired entry under a re-set key is reclaimed on the spot (one live slot per key); TTL arithmetic saturates instead of wrapping.
- `core/dev/env.c`: `env_init()` ignores a second call with a warning; a failed `bus_register` is logged.
- `core/mask.h`: added `env_mask_has()`; documented that `env_mask_flags()` returns shifted-down flags.
- `core/dev/env.c`, `ext/envtool/`: `list_all` no longer lists `SECRET` entries. Use `list_match(ENV_Q_FLAG(ENV_FLAG_SECRET), …)` for a full diagnostic dump.

- `core/main.c`: Replaced the blocking `shell_run()` call in `kmain()` with a centralized super-loop. The loop now unconditionally calls `timer_drain()`, polls the keyboard non-blockingly via `kbd_poll()`, feeds characters into the shell state machine, and yields with `cpu_sti_hlt()`. This fixes timer starvation when the shell is idle.
- `core/shell.c`, `core/shell.h`: Converted the shell from a blocking `read_line` loop to a non-blocking state machine (`shell_tick`). The shell no longer owns the main CPU loop; it yields immediately if no input is available, allowing background tasks and timers to execute.
- `core/tui.c`: Patched `tui_run()` to stop blocking on `kbd_wait()`. It now uses `kbd_poll()`, calls `timer_drain()` when idle, and uses `cpu_sti_hlt()` to yield the CPU. This prevents TUI applications (like `top` and `envtool`) from stalling the entire kernel and allows their refresh timers to fire reliably.
- `core/dev/keyboard.c`: (Implicit) Shifted the `timer_drain()` responsibility out of the keyboard driver's blocking wait loop and into the kernel super-loop and TUI engine, decoupling hardware polling from scheduling.

- **Hardening pass — `core/arch/` headers** (doc + latent-guard only, no behavior change on success paths):
  - `core/arch/vmm.h`: documented that `VMM_WRITE/USER/NX/GLOBAL/NOCACHE` are **internal interface bits, not architectural PTE positions**; noted `vmm_map`/`vmm_unmap` **refuse** (return `false`) inside inherited Limine huge pages rather than splitting; documented `vmm_unmap` does not free now-empty intermediate tables; documented `VMM_USER` on a newly created mapping is currently ineffective (`walk_pte(create=true)` allocates supervisor-only intermediates) — propagation deferred.
  - `core/arch/heap.h`: `kfree` doc now distinguishes build behavior — double-free panics in **all** builds (via the `b->free` check), header corruption only under `-DSYM_MEMDEBUG`; `krealloc` failure wording clarified to "original block remains allocated and unmodified"; shrink-in-place (`new_payload <= old_payload`) documented as returning the same pointer **without** shrinking the block.
  - `core/arch/cpu.h`: added `: "memory"` clobber to `cpuid`; added `::: "memory"` to `cpu_halt_forever`'s inner `hlt`; noted `insw`/`outsw` drop `volatile` (callers doing volatile MMIO must use inline asm directly).
  - `core/arch/pmm.h`: `pmm_alloc_contig` alignment documented (first frame 4 KiB-aligned, no larger guarantee); `pmm_get_stats().free_frames` documented as **exact**, incrementally maintained.
  - `core/arch/pmm.h` + `core/arch/heap.h`: `pmm_init` second-call behavior documented as warn-no-op (guard added in `.c`, see below).

- **Hardening pass — `core/arch/` `.c`** (guards + one truncation fix):
  - `core/arch/pmm.c`: `pmm_free` emits `klog_warning` on in-range already-free frames under `SYM_MEMDEBUG` only (fast build stays idempotent-silent); `pmm_init` now **warn-no-op on second call** (matching `env_init` precedent); one comment on `find_highest_usable` noting it trusts Limine's map to be well-formed (no `base+length` overflow clamp).
  - `core/arch/vmm.c`: `vmm_map` now `return false` if `phys & ~PTE_ADDR_MASK` instead of silently truncating bits above 51 (matters for MMIO + `VMM_NOCACHE`).
  - `core/arch/heap.c`: `heap_init` now **warn-no-op on second call** — the previous `chunks_head = NULL;` would have leaked every prior chunk; stale `// not good, wastes 16 bytes` comment in `chunk_create` replaced with an accurate note on the sentinel reservation (`first + first_size + HDR_SIZE == chunk end`), dead commented line removed.

- **Hardening pass — `core/bus.c` / `core/bus.h`**:
  - `bus.c`: manifest entries are now stamped with owner `BUS_BUS_ID` (the writer) rather than `dev->owner_id`, so `PINNED`'s "only owner may overwrite" rule is consistent on any future re-publish; `bus_publish_manifest` is now single-shot (`manifest_published` static flag, warn-no-op on repeat); `bus_init` resets `bus_ready` and `manifest_published` (the missing `bus_ready` reset meant a re-inited bus would never re-publish); empty-entry `owner_id` aligned with `bus_owner_id(NULL)`.
  - `bus.h`: documented that `ops` must match `class` and the bus does not verify this; expanded `bus_settle` (runs once, end of second wave, before shell — other contexts are a bug); clarified `owner_id` vs `class` vs env-mask `TYPE` are three different things despite numeric 0–9 collision; documented `bus_publish_manifest`'s `READONLY | PINNED` policy and single-shot nature; added forward declarations (`struct console_ops; … struct vfs_ops;`) so the header compiles without the device headers.

- **Hardening pass — `core/mask.h`, `core/dev/env.h`, `core/dev/env.c`**:
  - `core/mask.h`: `ENV_Q_OWNER`/`ENV_Q_TYPE` now mask their inputs (`& 0xFFu` / `& 0xFu`) so out-of-range ids cannot bleed into adjacent fields; documented that OR-ing two `ENV_Q_FLAG` terms requires **both** flags (AND), not "either."
  - `core/dev/env.c`: **`env_set`'s expired-same-key reclaim reordered after staging succeeds** — a `set()` that fails at staging (`ENV_NO_SPACE` from heap exhaustion) or at `env_make_room` no longer collects an expired same-key entry, making the header's "a failed `set()` has no side effects" **literally true**. Policy checks now run against a *live* existing entry only (`expired_same_key` flag); commit path unchanged. Added `env_emit` zero-size defense (`ns_max == 0` / `name_max == 0` early-return; callers already validate, this is belt-and-braces). Under `SYM_MEMDEBUG`, `env_mask_unaccount` now `PANIC`s on index underflow (`bit_cnt[b] == 0`) instead of silently skipping.
  - `core/dev/env.h`: documented `is_exist` (cheap, no budget consumed, expired reads as absent); documented `env_destroy` leaves the bus entry registered (static ops, subsequent calls return `ENV_NO_DEVICE`) and **must not** be called from a `timer_drain` callback (`timer_cancel` stops future sweeps but an in-flight `env_sweep` will reschedule itself before returning); documented `max_bytes` bounds **logical payload**, not heap usage (~3 NULs + one header per entry overhead).

- **Hardening pass — `core/dev/timer.h`, `core/dev/timer.c`**:
  - `core/dev/timer.c`: **`timer_after` deadline sum now saturates** — `deadline = (need > UINT64_MAX - ticks) ? UINT64_MAX : ticks + need` — so a saturated `ms_to_ticks_ceil` request no longer wraps to a past deadline (which previously fired on the next ISR instead of waiting effectively forever); `timer_sleep_ms` rejects unrepresentable durations (`need > UINT64_MAX - ticks`) with a debug warning instead of wrapping `target`; **`timer_sleep_ms` now `PANIC`s if called from a `timer_drain` callback** (`in_drain` check, all builds — the header previously promised a debug assert that didn't exist, and mis-described the failure as a "deadlock" when it would actually stall the drain); `timer_init` now **warn-no-op on second call** (the existing `irq_register` panic stays as a second line of defense); `timer_publish_task` logs `klog_warning` if the reschedule fails (mirroring `env_sweep`'s fallback); comment above `timer_drain` now states the three invariants that make its pre-IRQ-save reads safe (ISR only sets `ready` false→true; `cancel`/`drain` both thread-context; `in_drain` blocks reentrancy) — SMP or an ISR that clears `ready` would break them.
  - `core/dev/timer.h`: fixed `timer_sleep_ms`-from-callback rationale ("would stall the drain," not "would deadlock"); noted `timer_init` double-call is warn-no-op; one line above `timer_after` noting `timer_irq` is O(`TIMER_MAX_TASKS`) per tick.

- **Hardening pass — `ext/script/decimal.h`, `ext/script/decimal.c`**:
  - `ext/script/decimal.h`: documented that **`DEC_F_SIGNED` means signed semantics supported** (payload may carry a leading `-`), **not** a per-value sign bit — `dec_set` sets it by default and `dec_get` does not require it for a `-`-prefixed payload; arithmetic is sign-driven by the payload via `mag_of`. Documented that `dec_set` sets `importance` but **never** `PINNED` (callers wanting pinning use `env->set` directly); documented that reserved fields (`strict_overflow`, `DEC_F_SATURATE`) must be at their documented defaults and are rejected otherwise.
  - `ext/script/decimal.c`: `udivmod128` now early-returns on `d == 0` (defensive; all current callers pre-check, matches the boundary-defense pattern); `dec_align` now validates `dec_valid(a) && dec_valid(b)` at the top (public entry point, header says inputs must have `ok == true`); corrected the `dec_cmp` fallback comment ("scale gaps <= 18 (`DEC_MAX_SCALE`)", was "<= 10^18"); corrected `dec_get`'s buffer-size comment ("19 digits + sign + NUL = 21", was "20 … 22").

- `ext/explorer/explorer.c`: re-added vfs browser back from older `0.1.0` codebase.

- `config.h`, `core/`, `core/arch/`: codebase seen some minor tweaks and refinements, added new `class` and `id` definition.
- `core/dev/metric.c`, `core/dev/metric.h`: added new metric service, uses `vfs.c` as endpoints, uses `env` to publish file locations (like Linux `/proc` concept).
- `ext/top/top.c`: removed `keyboard` module, re-designed layout, doubled refresh rate to `2600`ms.

- `ext/explorer/explorer.c`: patched with extra panel, for file contents.
- `core/env.c`, `core/bus.c`, `core/tui.c`, `ext/fbcon/fbcon_manifest.c`: refined manifest `namespace` symbols to shorter versions for clarity (ex. `kernel.dev.console => console`)
- `core/dev/timer.c`, `core/dev/timer.h`: patched to expose `task` metric values.
- `ext/envtool/envtool.c`: remastered as stand-alone inspection tool.
- `ext/stresser/stresser.c`, `ext/stresser/stresser.h`: added as stand-alone stress tool.
- `core/dev/metric.c`, `core/dev/metric.h`: added `timer` endpoints , `task_pending, task_active`.

- `ext/proxy/proxy.c`, `ext/proxy/proxy.h`, `ext/proxy/proxytest.c`: reshaped and patched for better error handling, added proxy tester.
---

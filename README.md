# Symbiote OS (active development)

<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />   

**Symbiote** is a small x86-64 kernel where a monolithic core and its extensions live in the same address space,  
in deliberate symbiosis: the core provides safe, handle-based services; the extensions consume them without ever touching raw memory, pointers, or hardware.  

This repository (`0.1.*`) is the active development line of the `0.1.0 (alpha)` kernel, updated continuously.  

<img width="386" height="233" alt="bootlog" src="https://github.com/user-attachments/assets/32e9b4cc-99f9-4f3b-96f4-1c619dbfbb7f" />
<img width="386" height="233" alt="monitor" src="https://github.com/user-attachments/assets/eb5251d9-3333-40aa-a648-d9f64d62abfe" />


*early testing*  

**Acknowledgements**  
Symbiote was developed with assistance from Claude (Anthropic), DeepSeek, and CursorAI, used for architectural review, code generation, and audit passes.  
All code is reviewed and owned by the maintainer; the tools are collaborators, not authors.  

## Core Identity: A Monolithic Foundation  

- **Single address space, Ring 0 execution.**  
Core subsystems, PMM, VMM, kernel heap, and drivers for console, keyboard, timer, and VFS, all execute directly in Ring 0 within the same address space.
- **Zero-overhead inter-subsystem communication.**  
Subsystems talk to each other through a static software bus (`bus.c`):  
modules register ops structs at boot (console\_ops, timer\_ops, …), and every call afterwards is a plain,  
lock-free C function call, no IPC, no message passing, no copying.  

- **Extensions as first-class guests.**  
Extensions are self-contained modules, initialized via priority-ordered boot waves (`ext.c`/`cmd.c`),  
decoupled from the core but sharing its address space.  

- **Scriptable, not trusted.**  
A scripting layer shares the kernel's address space without sharing its privileges:
  - **Scriptable device calls**, scripts invoke device functions through a safe hand-off path instead of calling into the kernel directly.
  - **Typed, self-regulating storage**, `env.c` provides a bounded key-value store where scripted tasks deposit and retrieve data,  
                                        with TTLs, access budgets, and eviction policy built in.
  - **A hard capability boundary**, scriptable objects and the (planned) interpreter never receive raw pointers, memory addresses,  
                                    or hardware port access. They interact with the kernel strictly through opaque integer handles,  
                                    serialized byte payloads, and the ownership masks defined in `mask.h`.  



## Building

Requires a recent **GCC or Clang, GNU Make, and xorriso**.  
The first build fetches Limine and the freestanding headers, then compiles the kernel and assembles a hybrid BIOS/UEFI ISO.

```
make                # build everything, produce symbiote-x86_64.iso
make run-bios       # boot under QEMU, BIOS path
make run            # boot under QEMU, UEFI path
make run-serial     # boot headless, serial console to the terminal
make -C kernel size # report kernel size
```

Build with `CPPFLAGS=-DSYM_MEMDEBUG` to enable `heap.c` header magic, poison, and owner tagging.  
Build with `EXTENSIONS="hello"` (recursive lookup) for a headless image with only the smoke-test extension.

**heap.c:**

```
#ifdef SYM_MEMDEBUG
  #define BLOCK_MAGIC     0xC0FFEE5Aull
  #define POISON_FREE     0xDD
  #define POISON_ALLOC    0xAA
#else
  #define BLOCK_MAGIC     0ull
#endif
```

## Project Files &amp; Structure

```
kernel/
  └── src/
      ├── main.c                      # Limine->kmain() entrypoint, initializers
      ├── core/
      │   ├── arch/                   # Low-level architecture & hardware abstractions (absolute ground-zero)
      │   │   ├── boot.h              # Limine bootloader API vectors & boot validation
      │   │   ├── cpu.h               # CPU tables, CPUID, & TSC-clock management
      │   │   ├── serial.h            # COM1 serial driver (115200 baud)
      │   │   ├── gdt.h               # Global Descriptor Table (Ring 0 setup)
      │   │   ├── idt.h               # Interrupt Descriptor Table (256 vectors)
      │   │   ├── pic.h               # 8259 PIC driver & virtual wire platform
      │   │   ├── isr.h               # Interrupt Service Routines & isr_stubs.S wrapper
      │   │   ├── pmm.h               # Physical Memory Manager (frame allocation)
      │   │   ├── vmm.h               # Virtual Memory Manager (PMM-backed paging)
      │   │   └── heap.h              # Kernel heap (block headers & HHDM coalescing)
      │   │
      │   ├── dev/                    # Subsystem device drivers & abstractions
      │   │   ├── console.h           # Console device & PS/2 interface
      │   │   ├── keyboard.h          # Keyboard poller, reader
      │   │   ├── vfs.h               # Virtual File System (ramfs implementation)
      │   │   ├── timer.h             # Programmable interval clock controller
      │   │   └── env.h               # Environment variable key-value storage device (self-regulating variable store)
      │   │
      │   ├── bus.h                   # Bus registry & late device discovery manager
      │   ├── version.h               # Master version definition
      │   ├── config.h                # Master config definition
      │   ├── mask.h                  # Environment mask contract decoder and encoder
      │   ├── kprintf.h               # Formatted text output multiplexer
      │   ├── klog.h                  # Kernel logging extension (kern, info, warn, error)
      │   ├── ext.h                   # Extension module dispatcher
      │   ├── cmd.h                   # Command-line dispatcher & interpreter
      │   ├── tui.h                   # TUI (Text-based User Interface) engine
      │   └── shell.h                # Interactive Shell Manager
      ├── lib/                        # Interior library
      │   ├── mem.h                   # Direct memory operation library
      │   └── string.h                # String manipulation library
      ├── linker-scripts/             # build scripts, linkers, offset partitions
      └── ext/                        # Kernel extensions, drivers, and visual assets
          ├── fbcon/                  # Framebuffer Console extension (Flanterm backend)
          ├── proxy/                  # Bus extension module (native safe bus call proxy service)
          ├── cpu/                    # Query cpuid information
          ├── system/                 # built-in system commands
          ├── memory/                 # Heap, memory inspect and testers
          ├── env/                    # Environment command-line tool
          ├── envtool/                # Environment inspect and testers
          ├── script/                 # VM machine & library (native script/compiler/bytecode service)
          └── assets/                 # Static resources & embedded binary payloads
```

---

## Boot &amp; Core

The kernel boots from an ISO under both BIOS and UEFI, validating the Limine protocol revision at every startup.  
The memory map and HHDM offset are queried from the bootloader; ACPI, RSDP, and boot modules arrive through the same request mechanism.

## Memory is Home

**Physical:**  
A bitmap page frame allocator, grown from the bootloader's memory map.  
It hands out `4 KiB` frames and contiguous runs; the bitmap itself lives in usable RAM via the HHDM, so the allocator costs no statically reserved memory.

**Virtual:**  
4-level page tables are cloned from Limine, CR3 is taken over, and `W^X` is applied to kernel sections.  
A small API covers creating and modifying mappings in the current address space; `VMM_NOCACHE` is supported for MMIO.

**Heap:**  
A chunked free list with block headers and immediate coalescing.  
`kmalloc`, `kfree`, `kzalloc`, `krealloc`, growing in `256 KiB` chunks from the PMM.  
The debug build (`-DSYM_MEMDEBUG`) adds header magic, poison on free and alloc, and owner tagging.

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

## In-depth: 8259 PIC driver, Timer conversions

`core/timer.h`, `core/timer.c`:  

overflow-safe millisecond→tick conversions with an explicit rounding policy at every call site.  
  - `ms_to_ticks_floor`, largest tick count ≤ `ms`
  - `ms_to_ticks_ceil`, smallest tick count ≥ `ms`
  - `ms_to_ticks_round`, nearest tick count
  - `timer_sleep_ms` and `timer_after` now use `_ceil`, both always wanted "at least this long" semantics implicitly.

All three saturate on overflow, and treat `hz == 0` as "timer not initialized" (return 0).

## In-depth: Environment Storage, Bus Registry, Manifest and Device Discovery

`core/dev/env.c`, `core/dev/env.h`, `core/mask.h`, `core/bus.c`:  
The environment device is a bounded, fixed-capacity store of `(namespace, name) → value | [mask-contract]` entries,  
registered on the bus as `BUS_CLASS_ENV` and reached through `bus_env()`.  

<img width="1286" height="863" alt="Screenshot_2026-11-06_23-04-0833" src="https://github.com/user-attachments/assets/e0183475-a18f-4b84-88ac-85c97c193578" />  

Per-entry policy:  

- **access budget**, reclaimed synchronously when exhausted
- **time budget (TTL)**, reclaimed asynchronously by a timer-driven sweeper  
	(60,000 ticks at 1000 Hz is exactly 60 seconds, one minute).  
	Reads are exact whether or not the sweeper has run: an expired entry always reads as absent.  
    The TTL saturates, so it can never wrap into "already expired".  
- **importance**, `0–10`, higher is more valuable; it decides who may be evicted under space pressure
- **mask**, a 32-bit classification field that env enforces in part (the flags) and otherwise never interprets

`mask.h` defines the 32-bit field layout:

```
     31      24 23    20 19    16 15                 0
    +----------+--------+--------+-------------------+
    |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
    | (8 bits) |(4 bits)|(4 bits)|    (16 bits)      |
    +----------+--------+--------+-------------------+
```

**Types:**  

```c
#define ENV_TYPE_NONE          0u   /* opaque blob, unset */
#define ENV_TYPE_STRING        1u   /* printable ANSI/ASCII/UTF-8 */
#define ENV_TYPE_INT           2u   /* decimal integer */
#define ENV_TYPE_BOOL          3u   /* "0" or "1" */
#define ENV_TYPE_HEX           4u   /* hex formatted "0xABCDEF" */
#define ENV_TYPE_PATH          5u   /* filesystem path */
#define ENV_TYPE_ENUM          6u   /* one of a defined set */
#define ENV_TYPE_STRUCT        7u   /* structured data */
#define ENV_TYPE_BIN           8u   /* binary data */
```

**Flags:**  

```c
#define ENV_FLAG_READONLY      (1u << 16)  /* policy: one-shot, no overwrites */
#define ENV_FLAG_SECRET        (1u << 17)  /* policy: hide from default listings */
#define ENV_FLAG_CACHEABLE     (1u << 18)  /* policy: safe to recompute */
#define ENV_FLAG_PINNED        (1u << 19)  /* policy: immune to space-pressure eviction */
```

### Flag semantics

| Flag | Enforced behavior |
|------|-------------------|
| `READONLY` | Once set, the entry can never be overwritten, by anyone, the owner included. |
| `PINNED` | Never chosen as an eviction victim while live. Only the **owner** (the writer's mask owner must equal the entry's) may overwrite it. |
| `SECRET` | Hidden from `list`, `list_all`, and any query whose `care` does not include the `SECRET` bit. Reads by exact name are **not** hidden: env is not an access-control system, so anything exposing env to untrusted code (the proxy) must gate it. |
| `CACHEABLE` | Safe to recompute: evicted before non-cacheable entries of the same importance. |

> **Remark:**  
> `unset` is deliberately **not** gated by `READONLY` or `PINNED`: env cannot see who is calling, so a pinned entry "can still be unset (erased)".  
> The same goes for `drop_namespace`, which is core-only. Callers that face untrusted code must apply their own ownership gate (the proxy does).  
> An entry whose TTL has expired is gone for policy purposes too: its key may be claimed again.

### Eviction and failed writes

When the store is full, victims are preferred in this order:

1. **expired** entries (even `PINNED` ones: the TTL already won)
2. **lowest importance**, but only entries with importance **≤ the incoming write's** are eligible, so a low-importance write can never displace a more valuable entry
3. **`CACHEABLE`** before non-cacheable
4. **least recently accessed**, then name order

Live `PINNED` entries are never victims. A `set()` is **all-or-nothing**: the strings are staged first, then the store proves that enough *eligible* space exists, and only then evicts. A write that cannot succeed (`ENV_NO_SPACE`, `ENV_READONLY`, or heap exhaustion) leaves the store exactly as it was; it never costs another entry its slot.

### Querying by mask

There are two query forms, and they are **not** interchangeable:

- **care/want (exact)**: matches entries where `(mask & care) == want`. This is the correct way to query `OWNER` and `TYPE`, which are *numbers*, not bitsets.
- **subset (legacy)**: `list_by_mask` / `count_by_mask` match entries whose mask contains *all* bits of the query. This is only meaningful for `FLAGS` and `PRIVATE` bits: as a subset test, owner `1` also matches owners `3`, `5` and `7`.

`mask.h` provides `ENV_Q_OWNER(id)`, `ENV_Q_TYPE(t)` and `ENV_Q_FLAG(f)`, each of which expands to the `(care, want)` pair:

```c
const struct env_ops *env = bus_env();
size_t n;

env->count_match(ENV_Q_OWNER(BUS_VM16), &n);        /* exactly the entries owned by vm16 */
env->count_match(ENV_Q_TYPE(ENV_TYPE_INT), &n);     /* exactly the int-typed entries */
env->count_match(ENV_Q_FLAG(ENV_FLAG_PINNED), &n);  /* pinned entries */
```

Test flags with `env_mask_has(mask, ENV_FLAG_PINNED)`. `env_mask_flags()` returns the flags *shifted down* to `0..15` and must not be compared against the unshifted `ENV_FLAG_*` constants.

### Mask index

The store keeps an exact, incrementally maintained index of what is in it:

| Counter | Meaning |
|---------|---------|
| `bit_cnt[32]` | number of entries with each mask bit set |
| `active_mask_or` | the exact OR of every live mask (bit `b` is set ⇔ `bit_cnt[b] > 0`) |
| `owner_cnt[256]` | number of entries per `OWNER` value |
| `type_cnt[16]` | number of entries per `TYPE` value |

Every `set` and every reclaim updates these in `O(1)` per set bit, so the index is **never stale and never rescanned**: there is no dirty flag. A query is rejected in `O(1)`, without touching a single entry, when:

1. any bit of `want` is absent from `active_mask_or`, or
2. the query fully specifies `OWNER` and `owner_cnt[owner] == 0`, or
3. the query fully specifies `TYPE` and `type_cnt[type] == 0`.

For example, "does anything still exist for a decommissioned driver?" is one array lookup. The index is a **filter, never an answer**: entries that have expired but are not yet swept are still counted, so a non-zero counter means "a scan is required" and a zero counter means "provably nothing matches". It can cost a scan; it can never return a wrong result.

## In-depth: Software-based Decimals

`ext/script/decimal.c`, `ext/script/decimal.h`:  

The `decimal.h` used as co-processor with the `env` store's 32-bit mask to carry its type metadata out-of-band,  
exactly where the value string normally lives.  

1. **The Mask Layout:**  
   The 32-bit mask is divided into four fields: `OWNER`, `TYPE`, `FLAGS`, and `PRIVATE`.  
   The decimal module completely ignores the top 16 bits and takes total ownership of the bottom 16 (`PRIVATE`). 

3. **The Private Contract:**  
   The decimal module splits its 16-bit `PRIVATE` space into three parts: a `SCALE` (8 bits), a `ROUND` mode (4 bits), and `FLAGS` (4 bits).  
   The `DEC_P_MAKE` macro packs these together when storing a value, and the `DEC_P_SCALE`/`DEC_P_ROUND` macros unpack them when reading.  

5. **The Data Flow:**  
   When the kernel stores a decimal (`dec_set`), it encodes `75.00` as the integer string `"7500"`,  
   packs a scale of `2` and `HALF_UP` rounding into the mask's private area, and saves both to `env`.  
   When an application retrieves it (`dec_get`), it reads the mask and string, uses `parse_i64` to extract the magnitude (`7500`),  
   and reconstructs the final `struct dec` (magnitude `7500`, scale `2`).  

*The mask carries the metadata; the string carries the payload, the decimal module is a translator between an integer string and a formatted software float.*  

Decimal bit layout:
```c
#define DEC_P_SCALE(priv) (((priv) >> 8) & 0xFF)
#define DEC_P_ROUND(priv) (((priv) >> 4) & 0x0F)
#define DEC_P_FLAGS(priv) (((priv)) & 0x0F)
#define DEC_P_MAKE(s,r,f) ((((uint16_t)(s) & 0xFF) << 8) | \
                           (((uint16_t)(r) & 0x0F) << 4) | \
                            ((uint16_t)(f) & 0x0F))
```

All calculations rely entirely on safe integer math (`int64_t`/`__int128`) with explicit overflow checks and dynamic rounding modes.  
Usage is simple: call `dec_set()` to store a value with your target scale and signature configuration, and `dec_get()` to retrieve and decode it back.


## In-depth: Proxy

`ext/proxy/`:  

Inspired by .NET's `MarshalByRefObject`, the proxy layer is a safe-call boundary over the bus.  
Consumers lease a handle by name, call methods by opcode, and unbind:

```c
const struct proxy_ops *p = bus_proxy();
uint32_t h;
if (p->bind("console", &h) == PROXY_OK) {
    const char msg[] = "Hello, World!\0";
    p->call(h, PROXY_CONSOLE_WRITE, (const uint8_t *)msg, sizeof msg, NULL, 0, NULL);
    p->unbind(h);
}
```

## In-depth: fbcon extension, with (Debian-ported) PSF Glyph fonts

`ext/fbcon/`, `ext/assets/fonts.h`:

PSF glyphs converted from the Debian console fonts. Selectable families:

- **Western Latin**, `default`, `lat`, `iso`
- **IBM PC / DOS**, `cp…`, `alt`
- **Cyrillic &amp; Eastern European**, `Cyr`, `ruscii`, `koi8`, `Mik`
- **Greek**, `gr`, `greek`, `gr737`
- **Multi-script**, `LatArCyrHeb`
- **Regional / Asian**, `Goha`, `arm`, `tis`, `tcvn`, `viscii`
- **Accessibility**, `brl` (Braille)

> A free to use tool was designed to convert PSF to C-based header assets, located in `/tools/psf2header.py`  

---

# Third Party licenses

This software incorporates components from third-party projects, detailed below:

```
-------------------------------------------------------------------------------
1. Debian - PSF Console Fonts (https://packages.debian.org/sid/console-data)
-------------------------------------------------------------------------------
This is the Debian-Linux prepackaged version of the data for the Linux
Console Data.

This package was put together by Yann Dirson <dirson@debian.org>, from
sources you can get from:
 ftp://lct.sourceforge.net/pub/lct/

Packaging is copyright 1999-2001 Yann Dirson	<dirson@debian.org>
	     copyright 2001 Alcove		http://www.alcove.fr/

The Linux Console Tools data are derived from the data files provided
by the latest version of the kbd package, and are covered by the GPL
version 2.0 (see /usr/share/common-licenses/GPL-2 for details).

The Latin-1 *.psf fonts were developed by H. Peter Anvin, and the
Latin-2 ones by Pavel Zaboj.

Goha and GohaClassic:
  Donated by Yitna Firdyiwek <ybf2u@virgina.edu> of GohaTibeb Associates
  and may be restributed without restriction under the GNU GPL 2.0.

All other Ethiopic fonts:
  The collection in this directory donated for Linux distributions is
  otherwise maintained by Daniel Yacob <Daniel_Yacob_at_UNECA@un.org> and
  <yacob@padis.gn.apc.org>. The are redistributable under the GNU GPL version
  2.0.

-------------------------------------------------------------------------------
2. Limine Bootloader (https://github.com/limine-bootloader/limine)
-------------------------------------------------------------------------------
Copyright (c) 2019-2026 Mintsuki and contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN, OUT OF OR IN USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
3. flanterm (https://github.com/mintsuki/flanterm)
-------------------------------------------------------------------------------
Copyright (c) 2022-2026 Mintsuki and contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN, OUT OF OR IN USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

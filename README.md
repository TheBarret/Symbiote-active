# Symbiote OS (active development)

<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />   

**Symbiote** is a small x86-64 kernel where a monolithic core and its extensions live in the same address space,  
in deliberate symbiosis: the core provides safe, handle-based services; the extensions consume them without ever touching raw memory, pointers, or hardware.  

This repository (0.1.1) is the active development line of the 0.1.0 (alpha) kernel, updated continuously.  

<img width="1286" height="863" alt="bootlog" src="https://github.com/user-attachments/assets/4b3e7883-53c5-46db-89c3-70f45de58d56" />  

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



# Active changes & log 0.1.1

## 8259 PIC driver, Timer conversions

- `core/timer.h`, `core/timer.c`: overflow-safe millisecond→tick conversions with an explicit rounding policy at every call site.
  - `ms_to_ticks_floor`, largest tick count ≤ `ms`
  - `ms_to_ticks_ceil`, smallest tick count ≥ `ms`
  - `ms_to_ticks_round`, nearest tick count
  - `timer_sleep_ms` and `timer_after` now use `_ceil`, both always wanted "at least this long" semantics implicitly.

All three saturate on overflow, and treat `hz == 0` as "timer not initialized" (return 0).

## Environment Storage, Bus Registry, Manifest and Device Discovery

`core/env.c`, `core/env.h`, `core/mask.h`, `core/bus.c`:
The environment device is a bounded, fixed-capacity store of `(namespace, name) → value` entries, registered on the bus as `BUS_CLASS_ENV` and reached through `bus_env()`.

<img width="1286" height="863" alt="Screenshot_2026-11-06_23-04-0833" src="https://github.com/user-attachments/assets/e0183475-a18f-4b84-88ac-85c97c193578" />  

Per-entry policy:

- **access budget**, reclaimed synchronously when exhausted
- **time budget (TTL)**, reclaimed asynchronously by a timer-driven sweeper  
(60,000 ticks at 1000 Hz is exactly 60 seconds, one minute)
- **importance**, eviction order under space pressure (0–10, between persistent and volatile caches)
- **mask**, free-form classification bits, searchable via `list_by_mask` / `count_by_mask`

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

> **Remark:**  
> `FLAGS` bits (notably `READONLY`, `PINNED`) are one-shot writes, with one exception: for `PINNED`, only the owner of the object may modify this value or its parameters, though it can still be `unset` (erased).

- `bus.c` publishes the full catalog of the bus registry: `kernel.dev.<name>.*`.
- `fbcon` publishes console `column, row, width, height`: `kernel.dev.console.*`.
- `tui.c` gained `tui_fetch_resolution()` and `CP437` support.
- `ext/env/env.c`, `ext/envtool/`: the `env` command-line tool, plus `envtool` inspection and stress testing, with `CP437` support.
- `ext/fbcon/fbcon.c`: the terminal now uses the kernel's `heap->kmalloc` pool.
- `ext/memory/md.c`: memory dump tool, safely scans memory in both directions without faulting.

  
- `dev/keyboard.c` publishes keystroke vectors `keystrokes, chars, dropped, scancode, ticks, modifiers, last_char`: `kernel.dev.keyboard.*`.
- `dev/timer.c` publishes delta vectors `ticks, uptime_ms, hz`: `kernel.dev.timer.*`.
- `dev/env.c`: fixed a memory leak in `env_set`'s overwrite-flag logic.
- `ext/script/decimal.c`, `ext/script/vm16.c`: software-based decimals, and started with a LC-3-style 16-bit native VM extension.

- `dev/keyboard.c`, `dev/keyboard.h`: patched with new `bus_ops` initializer table, exposing `locks` and `drops` capabilities.
- `ext/script/decimal.c`, `ext/script/decimal.h`: patched and cleaned up.
- `ext/script/vm16.c`, `ext/script/vm16.h`: continuation of first LC-3 functions, has its own bus address and `bus_ops` capabilities.

## Software-based Decimals

`ext/script/decimal.c`, `ext/script/decimal.h`:

Software-only decimal arithmetic, no FPU or SSE instructions anywhere in the path.  
A signed integer payload is combined with a fractional scale factor stored out-of-band in environment `mask.h` metadata (`dec.<namespace>`).

<img width="984" height="497" alt="decimals" src="https://github.com/user-attachments/assets/cc779130-cd4d-4ab5-8b8e-e8c28ca5b623" />  


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

## fbcon extension: PSF Glyph fonts

`ext/fbcon/`, `ext/assets/fonts.h`:

PSF glyphs converted from the Debian console fonts. Selectable families:

- **Western Latin**, `default`, `lat`, `iso`
- **IBM PC / DOS**, `cp…`, `alt`
- **Cyrillic &amp; Eastern European**, `Cyr`, `ruscii`, `koi8`, `Mik`
- **Greek**, `gr`, `greek`, `gr737`
- **Multi-script**, `LatArCyrHeb`
- **Regional / Asian**, `Goha`, `arm`, `tis`, `tcvn`, `viscii`
- **Accessibility**, `brl` (Braille)

## Proxy

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

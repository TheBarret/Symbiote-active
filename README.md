# Symbiote-active
<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   
**Current goal is to implement layers that accommodate a custom interpreter**  

**Acknowledgements**  
Symbiote was developed with assistance from Claude (Anthropic), DeepSeek, and CursorAI, used for architectural review, code generation, and audit passes.  
All code is reviewed and owned by the maintainer; the tools are collaborators, not authors.  

<img width="1286" height="863" alt="demo" src="https://github.com/user-attachments/assets/aa6693f2-d1ef-4855-a9ed-fe4eedee4c7b" />  


## Project Files & Structure
```
kernel/
  └── src/
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
      │   └── shell.h                 # Interactive Shell Manager
      ├── lib/                        # Interior library
      │   ├── mem.h                   # Direct memory operation library
      │   └── string.h                # String manipulation library
      └── ext/                        # Kernel extensions, drivers, and visual assets
          ├── fbcon/                  # Framebuffer Console extension (Flanterm backend)
          ├── proxy/                  # Bus extension module (provides funneled access to bus objects with guardrails, for featured interpreter)
          ├── cpu/                    # Query cpuid information
          ├── system/                 # built-in system commands
          ├── memory/                 # Heap, memory inspect and testers
          ├── env/                    # Environment command-line tool
          ├── envtool/                # Environment inspect and testers
          ├── tokenizer/              # Tokenizer library (provides tokenization for featured interpreter)
          └── assets/                 # Static resources & embedded binary payloads

```

## Building

Requires a recent **GCC or Clang, GNU Make, and xorriso**.  
First build fetches Limine and the freestanding headers, then compiles the kernel and assembles a hybrid BIOS/UEFI ISO.  
```
  make                # build everything, produce symbiote-x86_64.iso
  make run-bios       # boot under QEMU, BIOS path
  make run            # boot under QEMU, UEFI path
  make run-serial     # boot headless, serial console to the terminal
  make -C kernel size # report kernel size
```

Build with `CPPFLAGS=-DSYM_MEMDEBUG` to enable heap header magic, poison, and owner tagging.  
Build with `EXTENSIONS="hello"` (looks for `hello.c`) for a headless image with only the smoke-test extension.  

# Changelog 0.1.1

## Boot & Core

Booting from an ISO under BIOS and UEFI, verified against the Limine protocol revision at every startup.  
Memory map and HHDM offset queried from the bootloader. additionally the ACPI, RSDP, and boot modules are queried via the same request mechanism.  

<img width="872" height="486" alt="bootsequence" src="https://github.com/user-attachments/assets/9bc9ffed-7564-4407-be3c-5eb930f5f766" />  

## Memory = Home

<img width="1286" height="863" alt="memory testers" src="https://github.com/user-attachments/assets/10ae4fe5-8b63-4fe1-ab94-d5d3e3efb092" />  

**Physical:**  
Bitmap page allocator initialized from the bootloader's memory map.  
Hands out `4 KiB` frames and contiguous runs, bitmap placed in usable RAM via HHDM, not statically reserved.  

**Virtual:**  
4-level page tables cloned from Limine, CR3 taken over, `W^X` applied to kernel sections, and a small API for creating,  
and modifying mappings in the current address space. `VMM_NOCACHE` supported for MMIO.  

**Heap:**  
Chunked free list with block headers and immediate coalescing. `kmalloc, kfree, kzalloc, krealloc`, grows by `256KiB` chunks from the PMM.  
Debug build (`-DSYM_MEMDEBUG`) adds header magic, poison on free and alloc, and owner tagging.  


## Core: timer conversions
`core/timer.h`, `core/timer.c`  

Added overflow-safe millisecond→tick conversions with explicit rounding policy at every call site:  

- `ms_to_ticks_floor`, largest tick count ≤ `ms`
- `ms_to_ticks_ceil`, smallest tick count ≥ `ms`
- `ms_to_ticks_round`, nearest tick count

`timer_sleep_ms` and `timer_after` now use `_ceil` ("at least this long" semantics both always wanted implicitly).  
All three saturate on overflow and treat `hz == 0` as "timer not initialized" (return 0).  

## Core: environment device
`core/env.c`, `core/env.h`, `core/mask.h`, `core/bus.c`  

A bounded, fixed-capacity store of `(namespace, name) → value` entries, registered on the bus as `BUS_CLASS_ENV` and reached through `bus_env()`.  

Per-entry policy:  
- **access budget**: reclaimed synchronously when exhausted
- **time budget (TTL)**: reclaimed asynchronously by a timer-driven sweeper (60,000 ticks at 1000 Hz is exactly 60 seconds or 1 minute)
- **importance**: eviction order under space pressure (rng: 0 = persistent - 10 = volatile)
- **mask**: free-form classification bits, searchable via `list_by_mask` / `count_by_mask`

`mask.h` defines the 32-bit field layout:  

```
     31      24 23    20 19    16 15                 0
    +----------+--------+--------+-------------------+
    |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
    | (8 bits) |(4 bits)|(4 bits)|    (16 bits)      |
    +----------+--------+--------+-------------------+
```

`bus.c`'s `bus_publish_manifest()` catalogues the entire registry into `kernel.dev.<name>.*` under the env store.  
`fbcon` publishes its own `column, row, width, height`; `tui_fetch_resolution()` reads it back.  

> **Remark:**  
> `FLAGS` bits (notably `READONLY`, `PINNED`) are one-shot write's, with the exception for `PINNED`,  
> where only the owner of the object can modify this value or parameters, but you can `unset` (erase) them.  

**Policy:**  
When a variable exists, the `env` performs a simple check:  
```c
    /* Overwrite protocol */
    if (variable != NULL) {
        uint32_t old_flags = env_mask_flags(existing->mask);
        uint32_t old_owner = env_mask_owner(existing->mask);
        uint32_t mask_arg = env_mask_owner(mask);
    
        /* PINNED: only the original owner may rewrite */
        if ((old_flags & ENV_FLAG_PINNED) && old_owner != env_mask_owner(mask_arg))
            return ENV_READONLY; 
    
        /* READONLY: one-shot-write, authentic values  */
        if (old_flags & ENV_FLAG_READONLY)
            return ENV_READONLY;
    }
```

- `ext/fbcon/fbcon.c`: terminal uses kernel's `heap->kmalloc` pool.

- `ext/memory/md.c`: memory dump tool, has safety-guards to scan memory up and down and not crash.
  ```
      > md scan
      Memory regions (index -> md <n> to dump):
        idx  start              end                type                   size  readable
        ---  ------------------ ------------------ ---------------- ---------- --------
        [ 0] 0x0000000000001000 0x0000000000073000 boot-reclaim         456 KiB  yes
        [ 1] 0x0000000000073000 0x000000000009f000 usable               176 KiB  yes
        [ 2] 0x000000000009fc00 0x00000000000a0000 reserved               1 KiB  no
        [ 3] 0x00000000000f0000 0x00000000000f5000 reserved              20 KiB  no
        [ 4] 0x00000000000f5000 0x00000000000f6000 reserved-mapped        4 KiB  yes
        [ 5] 0x00000000000f6000 0x0000000000100000 reserved              40 KiB  no
        [ 6] 0x0000000000100000 0x000000000fbd9000 usable               250 MiB  yes
        [ 7] 0x000000000fbd9000 0x000000000fbdd000 boot-reclaim          16 KiB  yes
        [ 8] 0x000000000fbdd000 0x000000000fe0f000 kernel                 2 MiB  yes
        [ 9] 0x000000000fe0f000 0x000000000ff48000 boot-reclaim           1 MiB  yes
        [10] 0x000000000ff48000 0x000000000ff61000 usable               100 KiB  yes
        [11] 0x000000000ff61000 0x000000000ffdf000 boot-reclaim         504 KiB  yes
        [12] 0x000000000ffdf000 0x000000000ffe0000 reserved               4 KiB  no
        [13] 0x000000000ffe0000 0x000000000ffe3000 reserved-mapped       12 KiB  yes
        [14] 0x000000000ffe3000 0x0000000010000000 reserved             116 KiB  no
        [15] 0x00000000b0000000 0x00000000c0000000 reserved             256 MiB  no
        [16] 0x00000000fd000000 0x00000000fd3e8000 framebuffer            3 MiB  yes
        [17] 0x00000000fed1c000 0x00000000fed20000 reserved              16 KiB  no
        [18] 0x00000000fffc0000 0x0000000100000000 reserved             256 KiB  no
        [19] 0x000000fd00000000 0x0000010000000000 reserved           12288 MiB  no

        ...inspecting region 8 (using an arbitrary length)....

        > md 8 16000
        Region 8 (kernel)  phys 0xfbdd000 + 0x3e80  ->  va 0xffff80000fbe0e80  (256 bytes)
        ffff80000fbe0e80  e2 4c 89 ee e8 b7 4f 00  00 85 c0 75 13 ba 03 00  |.L....O....u....|
        ffff80000fbe0e90  00 00 4c 89 f6 4c 89 ff  e8 43 fd ff ff 0f 1f 00  |..L..L...C......|
        ffff80000fbe0ea0  48 83 c3 01 49 3b 5f 08  72 b6 31 c0 48 83 c4 08  |H...I;_.r.1.H...|
        ffff80000fbe0eb0  5b 41 5c 41 5d 41 5e 41  5f 5d c3 b8 fb ff ff ff  |[A\A]A^A_]......|
        ffff80000fbe0ec0  eb ea b8 fa ff ff ff eb  e3 0f 1f 80 00 00 00 00  |................|
        ffff80000fbe0ed0  55 48 89 e5 41 57 41 56  41 55 41 54 53 48 83 ec  |UH..AWAVAUATSH..|
        ffff80000fbe0ee0  18 48 83 7f 08 00 48 89  75 c8 48 89 4d c0 0f 84  |.H....H.u.H.M...|
        ffff80000fbe0ef0  9c 00 00 00 49 89 fe 49  89 d4 4d 89 c5 31 db eb  |....I..I..M..1..|
        ffff80000fbe0f00  11 0f 1f 80 00 00 00 00  48 83 c3 01 49 3b 5e 08  |........H...I;^.|
        ffff80000fbe0f10  73 7e 49 8b 16 48 8d 04  db 4c 8d 3c c2 41 80 7f  |s~I..H...L.<.A..|
        ffff80000fbe0f20  40 00 74 e4 49 83 7f 28  00 74 11 49 8b 46 28 48  |@.t.I..(.t.I.F(H|
        ffff80000fbe0f30  85 c0 74 08 ff 10 49 3b  47 28 73 cc 41 8b 47 18  |..t...I;G(s.A.G.|
        ffff80000fbe0f40  4c 39 e0 75 c3 41 8b 47  1c 4c 39 e8 75 ba 49 8b  |L9.u.A.G.L9.u.I.|
        ffff80000fbe0f50  3f 48 8b 75 c8 4c 89 e2  e8 e3 4e 00 00 85 c0 75  |?H.u.L....N....u|
        ffff80000fbe0f60  a7 49 8b 7f 08 48 8b 75  c0 4c 89 ea e8 cf 4e 00  |.I...H.u.L....N.|
        ffff80000fbe0f70  00 85 c0 75 93 48 83 c4  18 4c 89 f8 5b 41 5c 41  |...u.H...L..[A\A|
  ```

## Shell Toolkits

`ext/env/env.c`, `ext/envtool/`:  

`env` command-line tool plus `envtool`, a CP437 TUI for browsing, inspecting, and stress-testing the environment store.  

`tui.c` gained `tui_fetch_resolution()` and a CP437 charset for primitive panels; `envtool` uses both.  

## Console: using custom fonts

`ext/fbcon/`, `ext/assets/fonts.h`:  

Converted PSF glyphs from the Debian console fonts. Selectable families:  

- **Western Latin**, `default`, `lat`, `iso`
- **IBM PC / DOS**, `cp…`, `alt`
- **Cyrillic & Eastern European**, `Cyr`, `ruscii`, `koi8`, `Mik`
- **Greek**, `gr`, `greek`, `gr737`
- **Multi-script**, `LatArCyrHeb`
- **Regional / Asian**, `Goha`, `arm`, `tis`, `tcvn`, `viscii`
- **Accessibility**, `brl` (Braille)

## Interpreter, stage 1
`ext/tokenizer/`, `ext/proxy/`  

**`tokenizer`**  
First stage of the interpreter, rrecognises data types `string`, `int`, `hex`;  
structures `symbolics`, `identifiers` (functions), and keywords (`use`, `let`, `while`, `if`, `else`).   

**`proxy`**  
A `MarshalByRefObject`-style safe-call layer over the bus.  

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

## Keyboard Polling

The keyboard is not on the `bus` but does write to the environment store;
- keystrokes total   (accumulative)
- chars total        (accumulative)
- dropped total      (overflow)
- scancode           (hexadecimal)
- ticks              (accumulative)
- last_char          (only printable keystrokes are recorded, no `CRLF` or `TAB`, etc)

`keyboard.c` uses the `timer_after()` to update every `n`-ticks (defined at: `config.h`):  

<img width="574" height="187" alt="env/kbd" src="https://github.com/user-attachments/assets/d1fa6d42-1ee2-41bd-af4b-7e5faf88c6c5" />

*env command-line query*  

<img width="925" height="439" alt="image" src="https://github.com/user-attachments/assets/1d0842c6-e10d-487b-a0ae-9588da771187" />  

*env inspection viewer (uses TUI engine)*  

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
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING FROM, OUT OF OR IN
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
    
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
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING FROM, OUT OF OR IN
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

# Symbiote-active
<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   
**Current goal is to implement layers that accommodate a custom interpreter**  

**Acknowledgements**  
Symbiote was developed with assistance from Claude (Anthropic), DeepSeek, and CursorAI, used for architectural review, code generation, and audit passes.  
All code is reviewed and owned by the maintainer; the tools are collaborators, not authors.  

<img width="1286" height="863" alt="boot" src="https://github.com/user-attachments/assets/9e86190d-187f-4c27-af27-4faec2393479" />  

## Project Files & Structure

<img width="872" height="486" alt="bootsequence" src="https://github.com/user-attachments/assets/9bc9ffed-7564-4407-be3c-5eb930f5f766" />

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
# Changelog 0.1.1

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

## Shell toolkit

`ext/env/env.c`, `ext/envtool/`  

`env` command-line tool plus `envtool`, a CP437 TUI for browsing, inspecting, and stress-testing the environment store.  

<img width="836" height="429" alt="envtool" src="https://github.com/user-attachments/assets/c91cea1f-afd3-4cf0-9895-2e22ff042a4e" />

`tui.c` gained `tui_fetch_resolution()` and a CP437 charset for primitive panels; `envtool` uses both.  

## Console: custom fonts
`ext/fbcon/`, `ext/assets/fonts.h`  

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

`keyboard.c` will update every `n`-ticks (defined at: `config.h`):  

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

# Symbiote-active
<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

**0.1.1** continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   

**Acknowledgements**  
Symbiote was developed with assistance from Claude (Anthropic), DeepSeek, and CursorAI, used for architectural review, code generation, and audit passes.  
All code is reviewed and owned by the maintainer; the tools are collaborators, not authors.  

## Core Identity: Monolithic Foundation

- Single Address Space & Ring 0 Execution:  
  Core subsystems (PMM, VMM, the kernel heap, drivers like console, keyboard, timer, and VFS) all execute directly in Ring 0 within the same address space.  

- Zero-Overhead Inter-Subsystem Communication:  
  Uses a static software bus (`bus.c`) where core modules register class-specific ops structs (console_ops, timer_ops, etc.),  
  allowing direct, lock-free C function calls after initialization.  

- Extension Architecture:  
  Extensions are designed to be decoupled into self-contained extension modules initialized (`ext.c/cmd.c`) via priority-ordered boot waves.  

- Hybrid Architecture:  
  A foundation has been created for a script component that can safely hand-off `device` function calls,
  and storage depositing for scripted tasks, combined with a self-regulating environment variable store (`env.c`) that deploys a separation of privileges,  
  Scriptable objects and/or the (planned) interpreter never gets raw pointers, memory addresses, or hardware port access.  
  It interacts with the kernel strictly through opaque integer handles, serialized byte payloads, and strict ownership masks (`mask.h`).   

<img width="1286" height="863" alt="demo" src="https://github.com/user-attachments/assets/aa6693f2-d1ef-4855-a9ed-fe4eedee4c7b" />  


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

## Project Files & Structure
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
---

## Boot & Core

Booting from an ISO under BIOS and UEFI, verified against the Limine protocol revision at every startup.  
Memory map and HHDM offset queried from the bootloader. additionally the ACPI, RSDP, and boot modules are queried via the same request mechanism.  

<img width="872" height="486" alt="bootsequence" src="https://github.com/user-attachments/assets/9bc9ffed-7564-4407-be3c-5eb930f5f766" />  

## Memory is Home

**Physical:**  
Bitmap page allocator initialized from the bootloader's memory map.  
Hands out `4 KiB` frames and contiguous runs, bitmap placed in usable RAM via HHDM, not statically reserved.  

**Virtual:**  
4-level page tables cloned from Limine, CR3 taken over, `W^X` applied to kernel sections, and a small API for creating,  
and modifying mappings in the current address space. `VMM_NOCACHE` supported for MMIO.  

**Heap:**  
Chunked free list with block headers and immediate coalescing.  
`kmalloc, kfree, kzalloc, krealloc`, grows by `256KiB` chunks from the PMM.  
Debug build (`-DSYM_MEMDEBUG`) adds header magic, poison on free and alloc, and owner tagging.  

<img width="1286" height="863" alt="memory testers" src="https://github.com/user-attachments/assets/10ae4fe5-8b63-4fe1-ab94-d5d3e3efb092" />  


# Changelog 0.1.1

## 8259 PIC driver, Timer conversions

- `core/timer.h`, `core/timer.c` added overflow-safe millisecond→tick conversions with explicit rounding policy at every call site.  
  - `ms_to_ticks_floor`, largest tick count ≤ `ms`
  - `ms_to_ticks_ceil`, smallest tick count ≥ `ms`
  - `ms_to_ticks_round`, nearest tick count
  - `timer_sleep_ms` and `timer_after` now use `_ceil`  
    ("at least this long" semantics both always wanted implicitly).  

All three saturate on overflow and treat `hz == 0` as "timer not initialized" (return 0).  


## Environment Storage, Bus Registry, Manifest and Device Discovery

`core/env.c`, `core/env.h`, `core/mask.h`, `core/bus.c`:  

<img width="898" height="454" alt="env" src="https://github.com/user-attachments/assets/375c2576-4679-4208-a55d-93ad811993b3" />  

The environment device is a bounded, fixed-capacity store of `(namespace, name) → value` entries,  
registered on the bus as `BUS_CLASS_ENV` and reached through `bus_env()`.  

Per-entry policy:  
- **access budget**: reclaimed synchronously when exhausted
- **time budget (TTL)**: reclaimed asynchronously by a timer-driven sweeper.  
  (60,000 ticks at 1000 Hz is exactly 60 seconds or 1 minute)  
- **importance**: eviction order under space pressure (range of: 0 - 10; between persistent and volatile caches)
- **mask**: free-form classification bits, searchable via `list_by_mask` / `count_by_mask`

  `mask.h` defines the 32-bit field layout:  
  
  ```
       31      24 23    20 19    16 15                 0
      +----------+--------+--------+-------------------+
      |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
      | (8 bits) |(4 bits)|(4 bits)|    (16 bits)      |
      +----------+--------+--------+-------------------+
  ```  
  > **Remark:**  
  > `FLAGS` bits (notably `READONLY`, `PINNED`) are one-shot write's, with the exception for `PINNED`,  
  > where only the owner of the object can modify this value or parameters, but you can `unset` (erase) them.  

  **Overwrite policy:**  

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

- `bus.c` publishes full catalog of the bus registry: `kernel.dev.<name>.*`.  

- `fbcon` publishes console `column, row, width, height`: `kernel.dev.console.*`.  

- `tui.c` added `tui_fetch_resolution()` and `CP437` support.  

- `ext/env/env.c`, `ext/envtool/`: `env` command-line tool, `envtool` inspection and stresser, added `CP437` support.  

- `ext/fbcon/fbcon.c`: terminal uses kernel's `heap->kmalloc` pool.

- `ext/memory/md.c`: memory dump tool, has safety-guards to scan memory up and down and not crash.
   <img width="417" height="301" alt="memory inspection" src="https://github.com/user-attachments/assets/aad5469c-2405-4897-b216-7ac0aba3be2d" />  

- `dev/keyboard.c`: publishes keystroke vectors `keystrokes, chars, dropped, scancode, ticks, modifiers, last_char`: `kernel.dev.keyboard.*`.  
  
- `dev/timer.c`: publishes delta vectors `ticks, uptime_ms, hz`: `kernel.dev.timer.*`.  

## fbcon extension: PSF Glyph fonts

`ext/fbcon/`, `ext/assets/fonts.h`:  

Converted PSF glyphs from the Debian console fonts. Selectable families:  

- **Western Latin**, `default`, `lat`, `iso`
- **IBM PC / DOS**, `cp…`, `alt`
- **Cyrillic & Eastern European**, `Cyr`, `ruscii`, `koi8`, `Mik`
- **Greek**, `gr`, `greek`, `gr737`
- **Multi-script**, `LatArCyrHeb`
- **Regional / Asian**, `Goha`, `arm`, `tis`, `tcvn`, `viscii`
- **Accessibility**, `brl` (Braille)

## Interpreter, stage 1 (Planned, busy on...)

`ext/tokenizer/`, `ext/proxy/`:  

**`tokenizer`**  
First stage of the interpreter, recognises data types `string`, `int`, `hex`;  
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

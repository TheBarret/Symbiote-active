# Symbiote-active
<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   

**Current goal is to implement layers that accommodate a custom interpreter**  

**0.1.1** - Major overhaul of the core

<img width="1286" height="863" alt="boot" src="https://github.com/user-attachments/assets/9e86190d-187f-4c27-af27-4faec2393479" />  


## Project Files & Structure

<img width="994" height="487" alt="boot sequence" src="https://github.com/user-attachments/assets/eea51fe3-f214-4d04-b571-320802b632d5" />



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


- All interior `core/*` modules have been refactored and aligned, and have added safety logic.
  
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

- `ext/envtool/`: added the `envtool` using the `TUI` engine to browse inspect and stress test the `environment` variable store.

- `mask.h`: added contracts and functions that can encode and decode the `MASK` bit field used in the `env.h`;  
  Bit layout:
  ```bash
        | 31    24 | 23  20 | 19  16 | 15              0 |
        +----------+--------+--------+-------------------+
        |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
        | (8 bits) | (4 bit)| (4 bit)|    (16 bits)      |
        +----------+--------+--------+-------------------+
   ````
  *(Flag bits are not enforced as of yet!)*

- `bus.c`: added `manifest` function, catalogues the entire bus registry, pushes information on the `environment` store.  
- `ext/fbcon/fbcon.c` : writes `column, row, width, height` manifest to `env` store.
- `tui.c`: added `tui_fetch_resolution()`, uses the `env` store to obtain framebuffer width and height.
- `tui.c`: added `CP437` charsets, for primitive panels.
- `ext/envtool/envtool.c`: added `CP437` support.

<img width="1286" height="863" alt="envtool" src="https://github.com/user-attachments/assets/867f6399-6500-4c16-807b-9f23b271023e" />  

- `ext/tokenizer/tokenizer.c`: added a token parser, first stage of the interpreter.  
  data-types: `string, int, hex`
  structures: `symbolics, identifiers = (functions), keywords = (use, let, while, if, else)`  

- `ext/proxy/proxy.h`, `ext/proxy/proxy.c`: added proxy module, inspired by Microsoft's [MarshalByRefobject](https://learn.microsoft.com/en-us/dotnet/api/system.marshalbyrefobject?view=net-10.0).  

  Example of writing `Hello, World!` to console device using safe call mechanism:  
  ```c
  const struct proxy_ops *p = bus_proxy();
  uint32_t h;
  /* lease  */
  if (p->bind("console", &h) == PROXY_OK) {
      const char msg[] = "Hello, World!\0";
      p->call(h, PROXY_CONSOLE_WRITE, (const uint8_t *)msg, sizeof msg, NULL, 0, NULL);
      p->unbind(h);
  }
  ```

- `/ext/assets/fonts.h`: added converted PSF font glyphs (Debian default console fonts).

- `/etc/fbcon/fbcon.c`: supports custom font glyps:
  * Western Latin (`default`, `lat`, `iso`)
  * IBM PC / DOS Code Page (`cp...`, `alt`)
  * Cyrillic & Eastern European (`Cyr`, `ruscii`, `koi8`, `Mik`)
  * Greek (`gr`, `greek`, `gr737`)
  * Multi-Script / Universal (`LatArCyrHeb`)
  * Regional & Asian Scripts (`Goha`, `arm`, `tis`, `tcvn`, `viscii`)
  * Accessibility & Special Purpose, Braille (`brl`)

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

# Symbiote Kernel

*Active development; interfaces and behavior may change.*  

**Symbiote OS** is a small, hobby, single-threaded x86-64 kernel written in C.  
It uses the Limine bootloader and is built as a hybrid BIOS/UEFI ISO.  

The design is guided by a simple premise: **everything on the kernel is expensive**.  
Memory, code, boot state, abstractions, and hardware support all have a real cost.   
The kernel therefore treats each host as a symbiotic partner: it uses Limine for the work it can safely delegate,   
retains only the facilities needed for a useful environment, and replaces the machinery of a general-purpose kernel with smaller, purpose-built contracts.  

That constraint is not an apology for incompleteness.  
It is the design method: implement a narrow interface, keep ownership obvious, avoid background machinery until it is needed, and make extensions opt-in.  
“Small” means fewer mechanisms, not less thoughtful engineering.  

<img width="1286" height="863" alt="bootlog" src="https://github.com/user-attachments/assets/47803f5f-4bb0-4198-88be-6e88c44a3ac3" />  

<img width="336" height="223" alt="screenshots" src="https://github.com/user-attachments/assets/f5f978ec-0f23-42d9-8583-e4de052c2b50" />
<img width="336" height="223" alt="screenshots" src="https://github.com/user-attachments/assets/1f9bd1b3-f0cf-4ac5-8bee-7c050d995026" />  

## What the kernel provides

### Boot and architecture

- Limine protocol boot validation and boot state handling.
- GDT, IDT, interrupt stubs, exception reporting, and a PIC-based interrupt platform.
- CPUID and TSC support with a small architectural abstraction layer.
- A COM1 serial driver for headless operation and diagnostics.
- A compact, 256-entry interrupt table and phase-oriented initialization.

### Memory

- **PMM:** a 4 KiB-frame physical memory manager backed by one bitmap, Limine memory maps, and the Higher Half Direct Map (HHDM).
- **VMM:** PMM-backed 4 KiB page mapping over a cloned Limine page table. Dynamic mappings use a reserved higher-half window.
- **Heap:** a 16-byte-aligned, first-fit kernel heap with block headers, immediate coalescing, `kmalloc`, `kzalloc`, `kfree`, and `krealloc`.
- Optional `SYM_MEMDEBUG` heap checking, including header magic, allocation/free poisoning, owner tagging, and heap validation.

The memory implementation is intentionally direct: PMM frames feed the heap and dynamic page mappings, while the VMM keeps host-provided HHDM and inherited mappings intact.

### Devices and storage

- PS/2 keyboard polling and text console support.
- Programmable Interval Timer support.
- A virtual filesystem interface with an in-RAM implementation.
- Byte-oriented file operations: open, close, read, write, seek, directory listing, create, unlink, and stat.
- A framebuffer console extension built on Flanterm.

The VFS is a backend interface rather than a promise of a disk implementation. The RAM filesystem supplied today is where files and directories live; a future backend is expected to provide its own block cache and disk behavior.

### Services and interface

- A small in-memory message router for unicast and multicast-style service communication.
- Fixed address conventions, callback sinks, bounded frame depth, frame validation, and multicast groups.
- Built-in console, random, loop, and greeter services.
- A cooperative loop with budgeted tasks for work such as draining messages and polling input.
- A line-oriented shell and command dispatcher.
- Commands are registered directly into linker sections; the shell has no runtime command inventory to maintain.
- Kernel logging with separate kernel, extension, service, info, warning, and error presentation.

This is the kernel’s “thin connective tissue”: modules register services and callbacks, then exchange small frames through addresses instead of introducing a network stack or a general IPC substrate.

### Extensions

A Symbiote extension is a small descriptor in a dedicated linker section. Extensions declare an initialization function and priority. The loader runs the descriptor levels in ascending order, allowing console, drivers, filesystems, services, applications, and built-in commands to participate in a clear startup sequence.

The default build includes:

- **fbcon:** framebuffer console using Flanterm.
- **cpuinfo:** CPUID-oriented inspection commands.
- **system:** built-in system commands.
- **memory:** memory testing and inspection tools.
- **tools:** extra testing commands.
- **assets:** fonts and embedded binary resources.

Extensions are compile-time modules. `EXTENSIONS="hello"` can select a smaller or alternate set of modules, but this is not a loaded-at-runtime plugin system.

## What Symbiote does not use

Symbiote is not intended to imitate every layer of a large general-purpose kernel. The following are not present or are not goals of the current design:

- **No virtual-memory process model.** There is no user-address-space API or process isolation implementation.
- **No Unix-style process or POSIX runtime.** The shell and commands run in the kernel environment.
- **No SMP scheduler.** The implementation is explicitly single-threaded; a production SMP design would require locking and substantially more machinery.
- **No preemptive userspace threading model.** Execution is organized by initialization phases and the cooperative loop.
- **No general-purpose networking stack.** The router is an in-kernel service bus, not an Ethernet, IP, TCP, or socket implementation.
- **No persistent filesystem or block storage system.** VFS behavior is RAM-backed today, with no disk cache or disk filesystem implementation in the core.
- **No dynamic module loader.** Extensions are selected at build time and linked into the image.
- **No full allocator framework.** The heap is a compact kernel allocator, not malloc-compatible runtime infrastructure.
- **No large hardware discovery platform.** Hardware support is intentionally limited to the devices needed by the booted kernel and enabled extensions.
- **No third-party C runtime abstraction.** The project is freestanding C with its own small memory, string, random, and symbol libraries.
- **No security claim.** This project is a systems experiment and hobby kernel, not a hardened isolation boundary or audited operating system.

These limits keep the system understandable and inexpensive. They also make Symbiote unsuitable as a drop-in replacement for Linux or another general-purpose operating system.

## Symbiotic design choices

| Host capability | Symbiote’s lightweight counterpart |
| --- | --- |
| Firmware and boot machinery | Limine supplies protocol data and memory/boot discovery; Symbiote validates what it needs. |
| Frame tracking | A bitmap over usable 4 KiB physical frames. |
| Address translation | A direct 4-level x86-64 mapping layer with 4 KiB dynamic mappings. |
| Kernel storage | Block headers and immediate heap coalescing instead of an allocator framework. |
| File storage | A one-interface, one-RAM-backend VFS instead of a persistent filesystem stack. |
| IPC and services | Fixed addresses and callback frames instead of a networking or microservice runtime. |
| Commands | Linker-registered command descriptors instead of a maintained command table. |
| Modules | Linker-registered, priority-ordered compile-time extensions instead of runtime plugins. |
| Output | Serial, PS/2 console, and opt-in framebuffer backends. |
| Debugging | Optional heap checks and panic context instead of a permanent diagnostic framework. |

## Project layout

```text
kernel/
├── src/
│   ├── main.c                 Limine entry point and staged initialization
│   ├── core/
│   │   ├── arch/              GDT, IDT, CPU, PIC, PMM, VMM, heap, serial
│   │   ├── dev/               Console, keyboard, timer, RAM-backed VFS
│   │   ├── services/          Router, addresses, I/O, random, and loop services
│   │   ├── ext.{h,c}          Linker-registered extension loader
│   │   ├── cmd.{h,c}          Shell command registry and dispatcher
│   │   ├── klog.{h,c}         Kernel logging
│   │   ├── kprintf.{h,c}      Formatted output
│   │   └── shell.{h,c}        Interactive shell manager
│   ├── lib/                   Memory, string, symbol, and random helpers
│   └── ext/                   Compile-time kernel extensions and assets
├── linker-scripts/            Kernel layout and linker sections
└── get-deps                   Dependency bootstrap script
GNUmakefile                    Build, ISO assembly, and QEMU targets
limine.conf                    Limine boot entry
```

## Building

Requirements: a recent GCC or Clang, GNU Make, and `xorriso`.

On the first build, the make process obtains Limine and the freestanding headers, then compiles the kernel and assembles a hybrid BIOS/UEFI ISO.

```sh
chmod +x kernel/get-deps
make                  # build symbiote-x86_64.iso
make run-bios         # boot through the BIOS path
make run              # boot through the UEFI path
make run-serial       # boot headlessly with serial in the terminal
make -C kernel size   # report kernel size
```

Build selected extensions with:

```sh
make EXTENSIONS="hello"
```

The default extension set is `fbcon system memory cpuinfo tools`.

Enable heap diagnostics with:

```sh
make CPPFLAGS=-DSYM_MEMDEBUG
```

The diagnostic build adds block magic, free/allocated poisoning, owner tagging, and heap checks. It changes runtime behavior and should be used when investigating allocator corruption rather than as the permanent default.

## Third-party components

Symbiote incorporates third-party components, including:

- **Limine Bootloader** for x86 BIOS/UEFI boot services and boot protocol data.
- **Flanterm** for the optional framebuffer console extension.
- **Debian PSF console fonts** for text console resources.
- **Freestanding C headers and Limine protocol code** supplied for a freestanding environment.
- **CC-Runtime contributors** for compiler runtime work referenced by the project.

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

-------------------------------------------------------------------------------
4. Freestanding C-headers / Limine Protocol codebase
-------------------------------------------------------------------------------

Copyright (C) 2022-2026 Mintsuki and contributors.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

-------------------------------------------------------------------------------
5. CC-RUNTIME
-------------------------------------------------------------------------------

This file is a partial list of people who have contributed to the LLVM/CompilerRT
project.  If you have contributed a patch or made some other contribution to
LLVM/CompilerRT, please submit a patch to this file to add yourself, and it will be
done!

The list is sorted by surname and formatted to allow easy grepping and
beautification by scripts.  The fields are: name (N), email (E), web-address
(W), PGP key ID and fingerprint (P), description (D), and snail-mail address
(S).

N: Craig van Vliet
E: cvanvliet@auroraux.org
W: http://www.auroraux.org
D: Code style and Readability fixes.

N: Edward O'Callaghan
E: eocallaghan@auroraux.org
W: http://www.auroraux.org
D: CMake'ify Compiler-RT build system
D: Maintain Solaris & AuroraUX ports of Compiler-RT

N: Howard Hinnant
E: hhinnant@apple.com
D: Architect and primary author of compiler-rt

N: Guan-Hong Liu
E: koviankevin@hotmail.com
D: IEEE Quad-precision functions

N: Joerg Sonnenberger
E: joerg@NetBSD.org
D: Maintains NetBSD port.

N: Matt Thomas
E: matt@NetBSD.org
D: ARM improvements.


==============================================================================
The LLVM Project is under the Apache License v2.0 with LLVM Exceptions:
==============================================================================

University of Illinois/NCSA
Open Source License

Copyright (c) 2009-2019 by the contributors listed in CREDITS.TXT

All rights reserved.

Developed by:

    LLVM Team

    University of Illinois at Urbana-Champaign

    http://llvm.org

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal with
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimers.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimers in the
      documentation and/or other materials provided with the distribution.

    * Neither the names of the LLVM Team, University of Illinois at
      Urbana-Champaign, nor the names of its contributors may be used to
      endorse or promote products derived from this Software without specific
      prior written permission.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
SOFTWARE.

==============================================================================

Copyright (c) 2009-2015 by the contributors listed in CREDITS.TXT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

```


## Acknowledgement

This is a hobby project. Core architecture and subsystem code are designed and reviewed by the maintainer;  
architecture and hardware support outside `/kernel/src/core/arch/` may be implemented according to the project’s own simplified contracts.  

Symbiote OS was developed with assistance from Claude (Anthropic), DeepSeek, and Cursor AI for architectural review, code generation, and audit passes.  
All code is reviewed and owned by the maintainer; these tools are collaborators, not authors.  

For usage questions or project discussion, join the maintainer on [Discord](https://discord.gg/bhDQwUxNn9).

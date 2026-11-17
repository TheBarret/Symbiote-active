# Symbiote-active

Simbiote, tiny hobby kernel made in C-lang, bootable x86-64.  

<img width="1286" height="863" alt="bootlog" src="https://github.com/user-attachments/assets/27e5477b-ac9b-4428-b61f-902fe6511208" />  


## Building

Requires a recent **GCC or Clang, GNU Make, and xorriso**.  

> The first build fetches Limine and the freestanding headers,  
> then compiles the kernel and assembles a hybrid BIOS/UEFI ISO.  
> Safety: `kernel/get-deps` needs `+x` before it works !!  

```
make                # build everything, produce symbiote-x86_64.iso
make run-bios       # boot under QEMU, BIOS path
make run            # boot under QEMU, UEFI path
make run-serial     # boot headless, serial console to the terminal
make -C kernel size # report kernel size
```
> Build with extensions `EXTENSIONS="hello"` (recursive build pattern: `ext/hello/hello.c`).  
> Build with header magic, poison, and owner tagging (`CPPFLAGS=-DSYM_MEMDEBUG` to enable `heap.c`).   

**heap.c, header magic, poison, and owner tagging:**  
```
#ifdef SYM_MEMDEBUG
  #define BLOCK_MAGIC     0xC0FFEE5Aull
  #define POISON_FREE     0xDD
  #define POISON_ALLOC    0xAA
#else
  #define BLOCK_MAGIC     0ull
#endif
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
```

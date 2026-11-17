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

Build with `CPPFLAGS=-DSYM_MEMDEBUG` to enable `heap.c` header magic, poison, and owner tagging.  
Build with `EXTENSIONS="hello"` (recursive lookup) for a headless image with only the smoke-test extension.  

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

/* Tell the linker that we want an x86_64 ELF64 output file */
OUTPUT_FORMAT(elf64-x86-64)

/* We want the symbol kmain to be our entry point */
ENTRY(kmain)

/* Define the program headers we want so the bootloader gives us the right */
/* MMU permissions; this also allows us to exert more control over the linking */
/* process. */
PHDRS
{
    limine_requests PT_LOAD FLAGS(0x06); /* Read, write */
    text PT_LOAD FLAGS(0x05); /* Read, execute */
    rodata PT_LOAD FLAGS(0x04); /* Read */
    data PT_LOAD FLAGS(0x06); /* Read, write */
}

SECTIONS
{
    /* We want to be placed in the topmost 2GiB of the address space, for optimisations */
    /* and because that is what the Limine spec mandates. */
    /* Any address in this region will do, but often 0xffffffff80000000 is chosen as */
    /* that is the beginning of the region. */
    . = 0xffffffff80000000;

    /* Define a section to contain the Limine requests and assign it to its own PHDR */
    .limine_requests : {
        __kernel_start = .;
        __limine_reqs_start = .;
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        __limine_reqs_end = .;
    } :limine_requests

    /* Move to the next memory page for .text */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    /* Move to the next memory page for .rodata */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
    } :rodata

    /* Symbiote extension descriptors. Every SYM_EXTENSION() lands here; the core */
    /* walks the table between the two symbols at boot. KEEP stops --gc-sections */
    /* from throwing away extensions nobody calls directly. */
    . = ALIGN(8);
    .symbiote_ext : {
        __symbiote_ext_start = .;
        KEEP(*(.symbiote_ext))
        __symbiote_ext_end = .;
    } :rodata
    /* Symbiote extension for interior built-in shell command discovery */
    .symbiote_cmd : ALIGN(8) {
        __symbiote_cmd_start = .;
        KEEP(*(.symbiote_cmd))
        __symbiote_cmd_end = .;
    }

    /* Add a .note.gnu.build-id output section in case a build ID flag is added to the */
    /* linker command. */
    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    /* Move to the next memory page for .data */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_end = .;

    .data : {
        __data_start = .;
        *(.data .data.*)
    } :data

    /* NOTE: .bss needs to be the last thing mapped to :data, otherwise lots of */
    /* unnecessary zeros will be written to the binary. */
    /* If you need, for example, .init_array and .fini_array, those should be placed */
    /* above this. */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_end = .;
    __kernel_end = .;

    /* Discard .note.* and .eh_frame* since they may cause issues on some hosts. */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}

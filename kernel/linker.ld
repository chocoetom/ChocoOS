ENTRY(_start)

OUTPUT_ARCH(i386:x86-64)
_kernel_start = 0xffffffff80000000;

SECTIONS {
    . = 0xffffffff80000000;

    /* NOTE: each permission class starts on its own 4K page.
       Limine refuses ELFs whose R+E / R / RW segments share a page. */
    .text : AT(ADDR(.text) - 0xffffffff80000000) {
        *(.text .text.*)
    }

    . = ALIGN(0x1000);

    .rodata : AT(ADDR(.rodata) - 0xffffffff80000000) {
        *(.rodata .rodata.*)
        /* Limine discovers requests by scanning FORWARD from the start
           marker to the end marker: order matters! */
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    }

    . = ALIGN(0x1000);

    .data : AT(ADDR(.data) - 0xffffffff80000000) {
        *(.data .data.*)
    }

    . = ALIGN(0x1000);

    .bss : AT(ADDR(.bss) - 0xffffffff80000000) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.comment)
        *(.note .note.*)
    }
}

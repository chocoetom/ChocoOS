/* Link Linux-ABI test programs like native ones (low half, ring 3). */
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}

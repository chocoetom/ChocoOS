/* Link user programs at 0x400000 (low half, ring-3 reachable). */
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}

SECTIONS
{
    KERNEL_OFFSET = 0xffffffc000000000;
    KERNEL_LOAD_PA = 0x00200000;

    . = KERNEL_OFFSET + KERNEL_LOAD_PA;
    _start = .;

    .text : AT(ADDR(.text) - KERNEL_OFFSET) {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_OFFSET) { *(.rodata .rodata.*) }

    .data : AT(ADDR(.data) - KERNEL_OFFSET) { *(.sdata .sdata.*) *(.data .data.*) }

    .bss : AT(ADDR(.bss) - KERNEL_OFFSET) {
        __bss_start = .;
        *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
        __bss_stop = .;
    }

    . = ALIGN(16);
    . = . + 0x8000;    /* 32 KB stack */
    _end = .;          /* stack pointer starts here */
}

SECTIONS
{
    . = 0x20000000; /* UART-loaded payload address on OrangePi RV2 */
    _start = .;

    .text : {
        *(.text.boot)
        *(.text)
    }

    .rodata : { *(.rodata) }

    .data : { *(.data) }

    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss)
        __bss_stop = .;
    }

    . = ALIGN(16);
    . = . + 0x8000;    /* 32 KB stack */
    _end = .;          /* stack pointer starts here */
}

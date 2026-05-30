SECTIONS
{
    . = 0x00200000; /* OrangePi RV2 load address */
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
    . = . + 0x8000; /* 32KB stack */
    _end = .;
}

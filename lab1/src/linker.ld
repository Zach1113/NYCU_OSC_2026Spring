SECTIONS
{
    . = 0x00200000; /* 0x80200000 for QEMU and 0x00200000 for OrangePi */
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

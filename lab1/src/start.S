.section ".text.boot"
.global _start

_start:
    /* Zero the .bss segment */
    la   a3, __bss_start
    la   a4, __bss_stop
    ble  a4, a3, clear_bss_done   /* skip if .bss is empty */
clear_bss:
    sd   zero, (a3)               /* write 8 bytes of zeros */
    add  a3, a3, 8
    blt  a3, a4, clear_bss
clear_bss_done:

    /* Set up the stack pointer */
    la   sp, _end

    tail start_kernel             /* jump to C (tail: no return) */

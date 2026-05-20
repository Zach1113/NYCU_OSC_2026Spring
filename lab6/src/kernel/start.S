.section ".text.boot"
.global _start

_start:
    mv   s0, a0                    /* hartid */
    mv   s1, a1                    /* physical FDT pointer */

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

    mv   a0, s1
    call setup_vm

    li   t0, 0xffffffc000000000     /* t0 = higher half offset */
    la   t1, higher_half_start
    add  t1, t1, t0
    jr   t1

higher_half_start:
    add  sp, sp, t0     /* adjust stack pointer for higher half */
    mv   a0, s0         /* pass hartid to C */
    add  a1, s1, t0     /* adjust FDT pointer for higher half */
    call drop_identity_map

    tail start_kernel             /* jump to C (tail: no return) */

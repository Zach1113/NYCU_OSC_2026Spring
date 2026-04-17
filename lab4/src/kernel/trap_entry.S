.section ".text"

.equ TF_RA,       0
.equ TF_SP,       8
.equ TF_GP,       16
.equ TF_TP,       24
.equ TF_T0,       32
.equ TF_T1,       40
.equ TF_T2,       48
.equ TF_S0,       56
.equ TF_S1,       64
.equ TF_A0,       72
.equ TF_A1,       80
.equ TF_A2,       88
.equ TF_A3,       96
.equ TF_A4,       104
.equ TF_A5,       112
.equ TF_A6,       120
.equ TF_A7,       128
.equ TF_S2,       136
.equ TF_S3,       144
.equ TF_S4,       152
.equ TF_S5,       160
.equ TF_S6,       168
.equ TF_S7,       176
.equ TF_S8,       184
.equ TF_S9,       192
.equ TF_S10,      200
.equ TF_S11,      208
.equ TF_T3,       216
.equ TF_T4,       224
.equ TF_T5,       232
.equ TF_T6,       240
.equ TF_SEPC,     248
.equ TF_SSTATUS,  256
.equ TF_SIZE,     272

.equ KC_RA,       0
.equ KC_SP,       8
.equ KC_GP,       16
.equ KC_TP,       24
.equ KC_S0,       32
.equ KC_S1,       40
.equ KC_S2,       48
.equ KC_S3,       56
.equ KC_S4,       64
.equ KC_S5,       72
.equ KC_S6,       80
.equ KC_S7,       88
.equ KC_S8,       96
.equ KC_S9,       104
.equ KC_S10,      112
.equ KC_S11,      120

.equ SSTATUS_SPP, 0x100

.balign 4
.global kernel_trap_vector
.global enter_user_mode

kernel_trap_vector:
    addi sp, sp, -8
    sd t0, 0(sp)
    csrr t0, sstatus
    andi t0, t0, SSTATUS_SPP
    beqz t0, .Ltrap_from_user

.Ltrap_from_kernel:
    addi sp, sp, -TF_SIZE  # reserve space for trap frame
    j .Lsave_trap

.Ltrap_from_user:
    csrr t0, sscratch
    csrw sscratch, sp
    mv sp, t0
    addi sp, sp, -TF_SIZE

.Lsave_trap:
    sd ra, TF_RA(sp)
    csrr t0, sstatus
    andi t0, t0, SSTATUS_SPP
    bnez t0, .Lsave_kernel_sp
    csrr t0, sscratch
    addi t0, t0, 8
    j .Lsave_sp_done
.Lsave_kernel_sp:
    addi t0, sp, TF_SIZE + 8
.Lsave_sp_done:
    sd t0, TF_SP(sp)
    sd gp, TF_GP(sp)
    sd tp, TF_TP(sp)
    sd t1, TF_T1(sp)
    sd t2, TF_T2(sp)
    csrr t2, sstatus
    andi t2, t2, SSTATUS_SPP
    bnez t2, .Lload_kernel_t0
    csrr t2, sscratch
    ld t0, 0(t2)
    j .Lsave_t0_done
.Lload_kernel_t0:
    ld t0, TF_SIZE(sp)
.Lsave_t0_done:
    sd t0, TF_T0(sp)
    sd s0, TF_S0(sp)
    sd s1, TF_S1(sp)
    sd a0, TF_A0(sp)
    sd a1, TF_A1(sp)
    sd a2, TF_A2(sp)
    sd a3, TF_A3(sp)
    sd a4, TF_A4(sp)
    sd a5, TF_A5(sp)
    sd a6, TF_A6(sp)
    sd a7, TF_A7(sp)
    sd s2, TF_S2(sp)
    sd s3, TF_S3(sp)
    sd s4, TF_S4(sp)
    sd s5, TF_S5(sp)
    sd s6, TF_S6(sp)
    sd s7, TF_S7(sp)
    sd s8, TF_S8(sp)
    sd s9, TF_S9(sp)
    sd s10, TF_S10(sp)
    sd s11, TF_S11(sp)
    sd t3, TF_T3(sp)
    sd t4, TF_T4(sp)
    sd t5, TF_T5(sp)
    sd t6, TF_T6(sp)

    csrr t0, sepc
    sd t0, TF_SEPC(sp)
    csrr t0, sstatus
    sd t0, TF_SSTATUS(sp)

    mv a0, sp
    call trap_handler

    ld t0, TF_SEPC(sp)
    csrw sepc, t0
    ld t0, TF_SSTATUS(sp)
    csrw sstatus, t0

    ld t0, TF_SSTATUS(sp)
    andi t0, t0, SSTATUS_SPP
    beqz t0, .Lreturn_user

.Lreturn_kernel:
    ld ra, TF_RA(sp)
    ld gp, TF_GP(sp)
    ld tp, TF_TP(sp)
    ld s0, TF_S0(sp)
    ld s1, TF_S1(sp)
    ld a0, TF_A0(sp)
    ld a1, TF_A1(sp)
    ld a2, TF_A2(sp)
    ld a3, TF_A3(sp)
    ld a4, TF_A4(sp)
    ld a5, TF_A5(sp)
    ld a6, TF_A6(sp)
    ld a7, TF_A7(sp)
    ld s2, TF_S2(sp)
    ld s3, TF_S3(sp)
    ld s4, TF_S4(sp)
    ld s5, TF_S5(sp)
    ld s6, TF_S6(sp)
    ld s7, TF_S7(sp)
    ld s8, TF_S8(sp)
    ld s9, TF_S9(sp)
    ld s10, TF_S10(sp)
    ld s11, TF_S11(sp)
    ld t3, TF_T3(sp)
    ld t4, TF_T4(sp)
    ld t5, TF_T5(sp)
    ld t6, TF_T6(sp)
    ld t0, TF_T0(sp)
    ld t1, TF_T1(sp)
    ld t2, TF_T2(sp)
    ld sp, TF_SP(sp)
    sret

.Lreturn_user:
    ld t0, TF_SP(sp)
    csrw sscratch, t0

    ld ra, TF_RA(sp)
    ld gp, TF_GP(sp)
    ld tp, TF_TP(sp)
    ld s0, TF_S0(sp)
    ld s1, TF_S1(sp)
    ld a0, TF_A0(sp)
    ld a1, TF_A1(sp)
    ld a2, TF_A2(sp)
    ld a3, TF_A3(sp)
    ld a4, TF_A4(sp)
    ld a5, TF_A5(sp)
    ld a6, TF_A6(sp)
    ld a7, TF_A7(sp)
    ld s2, TF_S2(sp)
    ld s3, TF_S3(sp)
    ld s4, TF_S4(sp)
    ld s5, TF_S5(sp)
    ld s6, TF_S6(sp)
    ld s7, TF_S7(sp)
    ld s8, TF_S8(sp)
    ld s9, TF_S9(sp)
    ld s10, TF_S10(sp)
    ld s11, TF_S11(sp)
    ld t3, TF_T3(sp)
    ld t4, TF_T4(sp)
    ld t5, TF_T5(sp)
    ld t6, TF_T6(sp)
    ld t0, TF_T0(sp)
    ld t1, TF_T1(sp)
    ld t2, TF_T2(sp)
    addi sp, sp, TF_SIZE
    csrrw sp, sscratch, sp
    sret

enter_user_mode:
    sd ra, KC_RA(a1)
    sd sp, KC_SP(a1)
    sd gp, KC_GP(a1)
    sd tp, KC_TP(a1)
    sd s0, KC_S0(a1)
    sd s1, KC_S1(a1)
    sd s2, KC_S2(a1)
    sd s3, KC_S3(a1)
    sd s4, KC_S4(a1)
    sd s5, KC_S5(a1)
    sd s6, KC_S6(a1)
    sd s7, KC_S7(a1)
    sd s8, KC_S8(a1)
    sd s9, KC_S9(a1)
    sd s10, KC_S10(a1)
    sd s11, KC_S11(a1)

    csrw sscratch, a2

    mv t5, a0
    ld t0, TF_SEPC(t5)
    csrw sepc, t0
    ld t0, TF_SSTATUS(t5)
    csrw sstatus, t0

    ld ra, TF_RA(t5)
    ld gp, TF_GP(t5)
    ld tp, TF_TP(t5)
    ld t0, TF_T0(t5)
    ld t1, TF_T1(t5)
    ld t2, TF_T2(t5)
    ld s0, TF_S0(t5)
    ld s1, TF_S1(t5)
    ld a0, TF_A0(t5)
    ld a1, TF_A1(t5)
    ld a2, TF_A2(t5)
    ld a3, TF_A3(t5)
    ld a4, TF_A4(t5)
    ld a5, TF_A5(t5)
    ld a6, TF_A6(t5)
    ld a7, TF_A7(t5)
    ld s2, TF_S2(t5)
    ld s3, TF_S3(t5)
    ld s4, TF_S4(t5)
    ld s5, TF_S5(t5)
    ld s6, TF_S6(t5)
    ld s7, TF_S7(t5)
    ld s8, TF_S8(t5)
    ld s9, TF_S9(t5)
    ld s10, TF_S10(t5)
    ld s11, TF_S11(t5)
    ld t3, TF_T3(t5)
    ld t4, TF_T4(t5)
    ld sp, TF_SP(t5)
    ld t6, TF_T6(t5)
    ld t5, TF_T5(t5)
    sret

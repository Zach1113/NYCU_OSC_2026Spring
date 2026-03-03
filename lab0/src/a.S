// Lab 0 - Minimal RISC-V bare-metal kernel
// Hardware: OrangePi RV2 (SpacemiT Ky X1, RV64GC)
//
// This is the simplest possible kernel.
// It enters an infinite low-power idle loop.
// Purpose: verify that the toolchain and boot pipeline work correctly.

.section ".text"
.global _start

_start:
    // wfi = Wait For Interrupt
    // Puts the RISC-V hart into a low-power sleep state.
    // The CPU wakes up when an interrupt is received,
    // then immediately loops back to sleep.
    wfi
    j _start

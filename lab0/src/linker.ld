/*
 * linker.ld — Linker script for NYCU OSC2026 Lab 0
 * Hardware: OrangePi RV2 (SpacemiT Ky X1)
 *
 * Memory Layout:
 *   The OrangePi RV2 boot sequence is:
 *     ROM → OpenSBI (M-mode firmware) → U-Boot → kernel
 *
 *   U-Boot loads the kernel FIT image into DDR and jumps to
 *   the entry point at 0x80200000 (S-mode, after OpenSBI sets up).
 *
 *   Section layout:
 *   ┌─────────────────┐  ← 0x80200000  (_start entry point)
 *   │      .text      │  executable code
 *   ├─────────────────┤
 *   │     .rodata     │  read-only data (string literals, constants)
 *   ├─────────────────┤
 *   │      .data      │  initialized read-write data (global variables)
 *   ├─────────────────┤
 *   │  .bss / .sbss   │  zero-initialized data (uninitialized globals)
 *   └─────────────────┘
 *
 * Note: .bss is NOT written into kernel.bin — the bootloader
 * guarantees the region is zeroed, or you must zero it manually
 * in a later lab when you set up the C runtime environment.
 */

SECTIONS
{
    /* Load address: where U-Boot places the kernel in DDR */
    . = 0x80200000;

    /* Code segment — must be first so _start is at the load address */
    .text : { *(.text) }

    /* Read-only data: string literals, const globals */
    .rodata : { *(.rodata) }

    /* Initialized data: global/static variables with non-zero initial values */
    .data : { *(.data) }

    /* Zero-initialized data: global/static variables initialized to 0 */
    .bss : { *(.sbss) *(.bss) }
}

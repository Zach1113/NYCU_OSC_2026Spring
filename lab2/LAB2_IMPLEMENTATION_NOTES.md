# Lab 2 Implementation Notes (Current Progress)

This document explains what has been implemented so far for Lab 2 and how it differs from Lab 1.

## Scope Completed

- Phase 0: Copy Lab 1 baseline into lab2 and make it build.
- Phase 1: UART bootloader command (`load`) with a simple transfer protocol.
- Phase 2: Flattened devicetree parser and runtime UART base initialization from DTB.
- Phase 3: Parse initrd address from DTB and implement cpio `ls`/`cat` commands.

The advanced self-relocation exercise is not implemented yet in this checkpoint.

## 1) What Changed from Lab 1 to Lab 2

### New command: `load`

In Lab 1, the shell only had:
- `help`
- `hello`
- `info`

In Lab 2 we added:
- `load`: receive a kernel image over UART and jump to it.

Main implementation is in:
- `src/main.c`

### New command: `uart`

To verify DTB parsing, we added:
- `uart`: prints the UART base address currently used by the driver.

Main implementation is in:
- `src/main.c`

### New commands: `initrd`, `ls`, `cat <name>`

To support Basic Exercise 3, we added:
- `initrd`: print initrd start/end/size parsed from `/chosen`.
- `ls`: list files from the cpio archive in initrd.
- `cat <name>`: print file content from cpio archive.

Main implementation is in:
- `src/main.c`
- `src/cpio.c`

### Runtime UART configuration (instead of only hardcoded base)

Lab 1 used compile-time hardcoded UART addresses.

Lab 2 now tries to read UART base from devicetree at runtime:
- Preferred node path: `/soc/serial`
- Fallback paths: `/soc/serial@10000000`, `/soc/serial@d4017000`
- Property: `reg`

If parsing fails, it keeps platform default fallback addresses.

Main implementation is in:
- `src/uart.c`

### New FDT parser module

Added minimal DTB parser functionality similar to ex21 goals:
- `fdt_path_offset(...)`
- `fdt_getprop(...)`
- Big-endian conversion helpers

Files:
- `src/fdt.h`
- `src/fdt.c`

### Host-side sender utility

Added a Python script to send kernel image through serial:
- `scripts/send_kernel.py`

Makefile helper target:
- `make send PORT=/dev/ttyUSB0`

## 2) UART Load Protocol Design

The transfer stream is:
1. 12-byte header (little-endian):
   - `magic` (u32): `0x544F4F42` ("BOOT")
   - `size` (u32): payload size in bytes
   - `checksum` (u32): sum of all payload bytes modulo `2^32`
2. Raw payload bytes (`size` bytes)

Validation steps on board:
- Verify `magic`.
- Verify `size` is non-zero and less than max allowed.
- Read payload into memory.
- Recompute checksum and compare.
- Jump to load address if validation passes.

## 3) Kernel Load Address Strategy

To avoid overwriting the running bootloader, we load to a high alternate address:

- OrangePi RV2 build: `0x20000000`
- QEMU build: `0x82000000`

This matches Lab 2 spec guidance for a non-overlapping load target before self-relocation is implemented.

## 4) Boot Argument and DTB Usage

On RISC-V boot entry:
- `a0`: hart id
- `a1`: DTB pointer

`start.S` already tail-calls C without clobbering `a0/a1`.

So `start_kernel(...)` in `main.c` now receives:
- `hartid`
- `fdt`

Then it calls:
- `uart_init_from_dtb(fdt)`
- `initrd_from_dtb(fdt)`

before interactive shell loop.

`initrd_from_dtb(fdt)` reads `/chosen`:
- `linux,initrd-start`
- `linux,initrd-end`

and passes this range to the cpio layer.

## 5) CPIO (newc) Support

The parser follows the new ASCII cpio format:
- Checks header magic (`070701`)
- Reads `c_namesize` and `c_filesize` (hex strings)
- Applies 4-byte alignment after name and after file data
- Stops traversal at `TRAILER!!!`

Implemented APIs:
- `cpio_set_archive(start, end)`
- `cpio_ready()`
- `cpio_ls()`
- `cpio_cat(filename)`

## 6) File-by-File Diff Summary

- `src/main.c`
  - Added UART load command and helpers.
  - Added `uart` command.
  - Added `initrd`, `ls`, and `cat <name>` shell commands.
  - Updated `start_kernel` signature to consume boot args.
  - Calls `uart_init_from_dtb(fdt)` during startup.
  - Parses initrd start/end from DTB `/chosen` properties.

- `src/cpio.h` (new)
  - Declares cpio archive APIs used by shell commands.

- `src/cpio.c` (new)
  - Implements newc header parsing, entry traversal, `ls`, and `cat`.

- `src/uart.c`
  - Reworked from pure hardcoded macros to runtime base handling.
  - Added DTB-based initialization using `/soc/serial` `reg` property.
  - Kept fallback defaults for safety.

- `src/fdt.h` (new)
  - Declares FDT constants/header and parser APIs.

- `src/fdt.c` (new)
  - Implements DTB path lookup and property retrieval.

- `src/uart.h` (new)
  - UART API declarations, including DTB init and base query.

- `scripts/send_kernel.py` (new)
  - Host utility to send header + payload over serial.

- `Makefile`
  - Added `send` target and `PORT` variable.
  - Added `run-initrd` target for QEMU with `-initrd initramfs.cpio`.
  - Updated `fit` target to include `initramfs.cpio` in FIT ramdisk image.

- `src/kernel.its`
  - Added `ramdisk` image section.
  - Added ramdisk binding in FIT configuration.

## 7) How to Use Current Implementation

1. Build:

```bash
make clean && make all
```

2. On board shell:
- Run `load`

3. On host:

```bash
make send PORT=/dev/ttyUSB0
```

4. Debug DTB UART base:
- Run `uart` in shell to print current base address.

5. Verify initrd + cpio (when booted with initrd):
- Run `initrd` to check DTB-derived range.
- Run `ls` to list archive entries.
- Run `cat <filename>` to print a file.

## 8) Known Remaining Work for Full Lab 2

- Basic Exercise 2/3 hardening:
  - Extend DTB parser coverage if additional nodes/properties are required.
  - Add more malformed-archive/path edge-case handling for cpio commands.

- Advanced Exercise:
  - Bootloader self-relocation.
  - Load kernel to standard entry (`0x00200000` board / `0x80200000` qemu) after relocation.

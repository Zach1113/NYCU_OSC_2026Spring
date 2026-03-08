## OSC 2026 – Lab 0 Implementation Walkthrough (Very Detailed)

This document explains **every important line and concept** in your Lab 0 implementation so you can confidently answer detailed questions from TAs.

You should read this together with:
- `a.S`
- `linker.ld`
- `kernel.its`
- `Makefile`
- `src/boot.cmd` (used on real hardware)

---

## 1. Big Picture: What Lab 0 Proves

- **Goal**: Verify that your **toolchain**, **linker script**, and **boot pipeline** all work end‑to‑end.
- **What actually runs on the CPU**: A tiny RISC‑V program that:
  - Enters a **low‑power sleep** (`wfi` = *Wait For Interrupt*), then
  - Jumps back to its own start address (`j _start`), forever.
- **Why this is enough**:
  - If you can reliably reach this code at the expected address, your:
    - Cross‑compiler toolchain is correctly installed.
    - Linker script places code where the firmware expects it.
    - QEMU / board boot flow is configured correctly.

### 1.1 Boot Chain on the OrangePi RV2

On real hardware, the boot chain is:

1. **ROM** (inside SoC, unchangeable)
2. **OpenSBI** (machine‑mode firmware)
3. **U‑Boot** (bootloader, supports FIT images)
4. **Your kernel** (this lab’s simple `a.S`)

Your Lab 0 work starts from step 4: you build a tiny “kernel” binary and package it so that **U‑Boot** can load and execute it.

---

## 2. The Minimal Kernel: `a.S`

File: `src/a.S`

```asm
.section ".text"
.global _start

_start:
    wfi
    j _start
```

Line‑by‑line:

- **`.section ".text"`**
  - Assembler directive: put the following instructions into the **`.text` section**.
  - Convention: `.text` holds **executable code**.
  - Your **linker script** (`linker.ld`) later decides **where in memory** this `.text` section will be placed.

- **`.global _start`**
  - Marks the symbol `_start` as **global** (visible outside this file).
  - The linker can then see `_start` and treat it as the **entry point** of your program (by placing it at the beginning of `.text`).
  - U‑Boot/OpenSBI will eventually jump to this address to start your kernel.

- **Label `_start:`**
  - Defines a **symbol** named `_start`.
  - Its value is “the address of the next instruction” (here, the `wfi`).
  - Because of the linker script, this address becomes the very first instruction at the load address (e.g. `0x80200000`).

- **`wfi`**
  - Stands for **Wait For Interrupt**.
  - RISC‑V privileged instruction.
  - Behavior:
    - Puts the **current hart (hardware thread/CPU core)** into a low‑power state.
    - The hart wakes up when an interrupt is pending.
  - In this lab, we do **not** configure interrupts, so:
    - On QEMU: you typically see the instruction being executed repeatedly in disassembly output as QEMU keeps stepping through.
    - On real hardware: the core is effectively parked in a low‑power loop.

- **`j _start`**
  - Unconditional **jump** to the symbol `_start`.
  - After executing `wfi`, execution continues to this instruction.
  - This instruction transfers control back to the label `_start`, forming an **infinite loop**:

    ```text
    _start:
        wfi
        j _start   ; jump back and repeat
    ```

Why this design is good for Lab 0:
- Very small (just two instructions).
- Easy to recognize in disassembly.
- Harmless: it does not touch memory, peripherals, or CSRs (beyond what `wfi` internally requires).

---

## 3. Controlling Memory Layout: `linker.ld`

File: `src/linker.ld`

This is a **linker script** for `riscv64-unknown-elf-ld`. It tells the linker:
- **Where the program should live in memory.**
- **How to arrange sections** like `.text`, `.rodata`, `.data`, `.bss`.

Key parts:

```ld
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
```

### 3.1 The Location Counter `.`

- **`. = 0x80200000;`**
  - Sets the **linker location counter** to `0x80200000`.
  - This means:
    - The **first byte** of the next section (`.text`) will be at **address `0x80200000`** in the linked ELF.
  - So:
    - `_start` (the first symbol in `.text`) ends up at `0x80200000`.
  - This address must **match what the bootloader / firmware expects**.

### 3.2 Section Placement

- **`.text : { *(.text) }`**
  - Create an output section named `.text`.
  - Put **all input sections** named `.text` from **all object files** into this output section.
  - The `*` wildcard matches all files; `(.text)` matches their `.text` sections.
  - Your `_start` code from `a.S` is in `.text`, so it lands here.

- **`.rodata : { *(.rodata) }`**
  - Collect all **read‑only data** (e.g., string literals, `const` globals).

- **`.data : { *(.data) }`**
  - Collect all **initialized global/static variables** with **non‑zero** initial values.
  - These are stored in the binary image and copied to RAM at startup in more complex kernels.

- **`.bss : { *(.sbss) *(.bss) }`**
  - Collect all **zero‑initialized** globals and statics.
  - `.bss` is typically **not stored** in the binary; instead, the runtime code just **zeros the region** at boot.
  - In this lab, there are no such variables, but we set it up correctly for later labs.

### 3.3 Why the Linker Script Matters

If the linker script is wrong, your code might be:
- Placed at the **wrong address**, so the bootloader jumps into garbage.
- Overlapping with other data (like the device tree or firmware data structures).

Lab 0’s linker script guarantees:
- `_start` is at `0x80200000`.
- Future `.rodata`, `.data`, `.bss` will be laid out **contiguously** after `.text`.

---

## 4. Packaging for U‑Boot: `kernel.its`

File: `src/kernel.its`

This file describes a **FIT image** (Flattened Image Tree), which is a U‑Boot format that can bundle:
- Kernel image
- Device tree (DTB)
- Optional RAM disk, etc.

Main structure:

```dts
/dts-v1/;
/ {
    description = "U-boot FIT Image for Orange Pi RV2";
    #address-cells = <2>;

    images {
        kernel {
            description = "Kernel Image";
            data = /incbin/("kernel.bin");
            type = "kernel";
            arch = "riscv";
            os = "linux";
            compression = "none";
            load = <0x0 0x00200000>;    /* physical load address */
            entry = <0x0 0x00200000>;   /* entry point = _start  */
        };

        fdt {
            description = "Flat Device Tree - OrangePi RV2";
            data = /incbin/("x1_orangepi-rv2.dtb");
            type = "flat_dt";
            arch = "riscv";
            compression = "none";
            load = <0x0 0x31000000>;    /* loaded high in DDR, away from kernel */
        };
    };

    configurations {
        default = "config-1";
        config-1 {
            description = "NYCU OSC2026 RISC-V Kernel";
            kernel = "kernel";
            fdt = "fdt";
        };
    };
};
```

### 4.1 Root Node

- **`/dts-v1/;`**
  - Marks this as a **Device Tree Source v1** file.

- **`/ { ... };`**
  - Root node of the FIT description.
  - Contains `images {}` and `configurations {}`.

- **`description = "U-boot FIT Image for Orange Pi RV2";`**
  - Human‑readable description; visible in U‑Boot tools.

- **`#address-cells = <2>;`**
  - States that addresses are represented using **two 32‑bit cells** (i.e., 64‑bit addresses) because this is a 64‑bit RISC‑V system.

### 4.2 The `images` Node

Contains two sub‑nodes: `kernel` and `fdt`.

#### 4.2.1 `kernel` node

- **`data = /incbin/("kernel.bin");`**
  - Embed the **contents of `kernel.bin`** (your raw binary) directly into the FIT image.
  - So, when you run `mkimage -f src/kernel.its kernel.fit`, this binary becomes part of `kernel.fit`.

- **`type = "kernel";`**
  - Tells U‑Boot “this image is a kernel.”

- **`arch = "riscv";`**
  - Architecture type; ensures U‑Boot treats it as RISC‑V.

- **`os = "linux";`**
  - OS type. For this bare‑metal lab, this is mostly a **convention**; U‑Boot still just jumps to the entry point.

- **`compression = "none";`**
  - The kernel is **not compressed**; U‑Boot doesn’t need to decompress it.

- **`load = <0x0 0x00200000>;`**
  - **Physical load address** (64‑bit, split in two 32‑bit cells).
  - Tells U‑Boot where in RAM to copy `kernel.bin` before booting.

- **`entry = <0x0 0x00200000>;`**
  - Address where U‑Boot should **jump to start execution**.
  - This should correspond to your `_start` symbol **after any virtual‑to‑physical mapping** used by OpenSBI/U‑Boot.
  - Board support code / course docs explain the exact mapping; conceptually:
    - The firmware ensures that your `_start` ends up at the address given by `entry`.

#### 4.2.2 `fdt` node

Describes the **device tree blob** (DTB).

- **`data = /incbin/("x1_orangepi-rv2.dtb");`**
  - Embed the binary DTB file (describes CPU, memory, devices).

- **`type = "flat_dt";`**
  - Flat device tree.

- **`load = <0x0 0x31000000>;`**
  - Physical address where the DTB will be placed.
  - Chosen so it does **not overlap** with the kernel’s memory region.

### 4.3 The `configurations` Node

```dts
configurations {
    default = "config-1";
    config-1 {
        description = "NYCU OSC2026 RISC-V Kernel";
        kernel = "kernel";
        fdt = "fdt";
    };
};
```

- Describes **boot configurations** that U‑Boot can choose.
- `default = "config-1";` selects `config-1` as the default.
- In `config-1`:
  - `kernel = "kernel";` selects the `images/kernel` node.
  - `fdt = "fdt";` selects the `images/fdt` node.

So, when U‑Boot is asked to boot this FIT image:
- It loads the kernel and DTB according to the `kernel` and `fdt` nodes.
- Then jumps to the kernel entry point.

---

## 5. Build Automation: `Makefile`

File: `Makefile`

This file defines **how to build**:
- `kernel.bin` (for QEMU).
- `kernel.fit` (for real hardware).
- Helper targets: `qemu`, `debug`, `gdb`, `disasm`, `clean`, etc.

### 5.1 Toolchain Variables

```make
CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
```

- `CROSS` is a **prefix** for all RISC‑V cross‑tools.
  - Example: `riscv64-unknown-elf-gcc`, `riscv64-unknown-elf-ld`, etc.
- `CC`, `LD`, `OBJCOPY`, `OBJDUMP` are convenience variables using this prefix.

### 5.2 Compiler Flags

```make
CFLAGS   = -Wall -nostdlib -nostartfiles -ffreestanding
ASMFLAGS =   # (empty for now)
```

- **`-Wall`**: Enable most warnings; helps catch mistakes.
- **`-nostdlib`**:
  - Do **not** link the standard C library (`libc`).
  - Appropriate for **bare‑metal** code; there is no OS providing system calls.
- **`-nostartfiles`**:
  - Do **not** use default startup files like `crt0.o`.
  - You provide your **own entry point** (`_start` in `a.S`).
- **`-ffreestanding`**:
  - Tell the compiler this is a **freestanding environment** (no hosted OS, no assumptions about standard library availability).

### 5.3 Source and Object Files

```make
SRCDIR   = src/
ASMFILES = $(wildcard $(SRCDIR)*.S)
CFILES   = $(wildcard $(SRCDIR)*.c)
OBJFILES = $(ASMFILES:.S=.o) $(CFILES:.c=.o)
```

- `SRCDIR` is the directory containing source files.
- `ASMFILES`:
  - Uses `wildcard` to find all `*.S` files under `src/` (currently `src/a.S`).
- `CFILES`:
  - Prepared for future C files (none yet in Lab 0).
- `OBJFILES`:
  - Converts `file.S` → `file.o`, `file.c` → `file.o`.
  - All these `.o` files will be linked into `kernel.elf`.

### 5.4 Linker Script Variable

```make
LDSCRIPT = $(SRCDIR)linker.ld
```

- Points to your custom linker script used when linking `kernel.elf`.

### 5.5 Default Target

```make
.PHONY: all
all: kernel.bin
```

- When you run `make` with no arguments, it builds `kernel.bin`.
- `kernel.bin` is a **raw binary** suitable for QEMU’s `-kernel` option.

### 5.6 Build Rules

#### 5.6.1 Assemble `.S` → `.o`

```make
$(SRCDIR)%.o: $(SRCDIR)%.S
	$(CC) $(CFLAGS) $(ASMFLAGS) -c $< -o $@
```

- Pattern rule:
  - For any `src/name.S`, produce `src/name.o`.
- Uses `gcc` as the **driver** even for assembly (`-c` means “compile only”).
- `$<` is the **input** file, `$@` is the **output** file.

#### 5.6.2 Compile `.c` → `.o`

```make
$(SRCDIR)%.o: $(SRCDIR)%.c
	$(CC) $(CFLAGS) -c $< -o $@
```

- Similar pattern rule for future C files.

#### 5.6.3 Link `.o` → `kernel.elf`

```make
$(SRCDIR)kernel.elf: $(LDSCRIPT) $(OBJFILES)
	$(LD) -T $(LDSCRIPT) $(OBJFILES) -o $@
	@echo "[LD]  $@"
```

- Invokes the **linker** directly:
  - `-T $(LDSCRIPT)` tells `ld` to use your `linker.ld`.
  - `$(OBJFILES)` are the input `.o` files.
  - Output is `src/kernel.elf`.
- The `@echo` line prints a nice message without echoing the command.

#### 5.6.4 Strip ELF → Raw Binary

```make
kernel.bin: $(SRCDIR)kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "[BIN] $@ ($(shell wc -c < $@) bytes)"
```

- `objcopy` converts the ELF file (with symbols, sections, metadata) into a **flat binary**:
  - No ELF headers, just the data that would live in memory at runtime.
- This is what QEMU uses as `-kernel kernel.bin`.
- It also prints the **size in bytes** for reference.

### 5.7 Building a FIT Image (`make fit`)

```make
.PHONY: fit
fit: kernel.bin
	@if [ ! -f $(SRCDIR)x1_orangepi-rv2.dtb ]; then \
		echo ""; \
		echo "ERROR: $(SRCDIR)x1_orangepi-rv2.dtb not found!"; \
		echo "Download it from:"; \
		echo "  https://github.com/nycu-caslab/OSC2026/raw/main/uploads/x1_orangepi-rv2.dtb"; \
		echo "Then place it in $(SRCDIR)"; \
		echo ""; \
		exit 1; \
	fi
	@# mkimage requires kernel.bin and DTB to be in the same directory as the .its file
	cp kernel.bin $(SRCDIR)kernel.bin
	mkimage -f $(SRCDIR)kernel.its kernel.fit
	rm -f $(SRCDIR)kernel.bin
	@echo "[FIT] kernel.fit ready — copy to SD card boot partition"
```

Step‑by‑step:

1. **Prerequisite**: `kernel.bin` must already exist.
2. **Check DTB presence**:
   - If `src/x1_orangepi-rv2.dtb` is missing, print an error and exit.
3. **Prepare files**:
   - `mkimage` expects `kernel.bin`, `kernel.its`, and the DTB in the **same directory**.
   - So: copy `kernel.bin` into `src/` as `src/kernel.bin`.
4. **Create FIT image**:
   - `mkimage -f src/kernel.its kernel.fit`:
     - Reads the `.its` description.
     - Produces `kernel.fit` in the top‑level directory.
5. **Cleanup**: remove the temporary `src/kernel.bin`.
6. **Result**:
   - `kernel.fit` is ready; you can place it on a boot medium for the OrangePi RV2.

### 5.8 QEMU Run Target (`make qemu`)

```make
.PHONY: qemu
qemu: kernel.bin
	qemu-system-riscv64 \
		-M virt \
		-kernel kernel.bin \
		-display none \
		-d in_asm \
		-nographic
```

Flags:
- **`-M virt`**:
  - Use the generic RISC‑V **virt** machine (virtual board).
- **`-kernel kernel.bin`**:
  - Load your raw binary as the “kernel”.
  - QEMU will place it at the default reset address for this board model.
- **`-display none`** and **`-nographic`**:
  - Disable graphical display; everything goes to the terminal.
- **`-d in_asm`**:
  - Dump instruction disassembly as QEMU executes.
  - You should see a repeating pattern:

    ```text
    0x80200000:  wfi
    0x80200004:  j 0x80200000
    ```

### 5.9 Debugging with GDB (`make debug`, `make gdb`, `make debug-build`)

- **`make debug`**:

  ```make
  debug: kernel.bin
  	@echo "QEMU paused. Connect GDB in another terminal with: make gdb"
  	qemu-system-riscv64 \
  		-M virt \
  		-kernel kernel.bin \
  		-display none \
  		-S -s \
  		-nographic
  ```

  - `-S`: Start QEMU with the CPU halted at reset; wait for debugger.
  - `-s`: Open a GDB server on TCP port 1234.
  - You then run `make gdb` to attach.

- **`make gdb`**:

  ```make
  gdb: $(SRCDIR)kernel.elf
  	@GDB=$$(command -v gdb-multiarch || command -v riscv64-elf-gdb || command -v riscv64-unknown-elf-gdb); \
  	if [ -z "$$GDB" ]; then \
  		echo "ERROR: no suitable GDB found ..."; \
  		exit 1; \
  	fi; \
  	echo "Using GDB: $$GDB"; \
  	$$GDB -ex "file $(SRCDIR)kernel.elf" \
  	      -ex "target remote :1234" \
  	      -ex "layout asm"
  ```

  - Locates a suitable GDB binary.
  - Loads `kernel.elf` as the debug symbol file.
  - Connects to QEMU’s GDB server on port `1234`.
  - Sets the UI to show the assembly layout.

- **`make debug-build`**:

  ```make
  debug-build: CFLAGS += -g
  debug-build: ASMFLAGS += -g
  debug-build: kernel.bin
  ```

  - Adds `-g` to both `CFLAGS` and `ASMFLAGS` to include debug symbols.
  - Rebuilds `kernel.bin` with full debug info.

### 5.10 Other Utility Targets

- **`make disasm`**:

  ```make
  disasm: $(SRCDIR)kernel.elf
  	$(OBJDUMP) -d $<
  ```

  - Disassembles `kernel.elf` without running QEMU.

- **`make clean`**:

  ```make
  clean:
  	rm -f $(SRCDIR)*.o $(SRCDIR)*.elf $(SRCDIR)kernel.bin
  	rm -f kernel.bin kernel.fit
  	@echo "[CLEAN] Done"
  ```

  - Removes all build artifacts.

---

## 6. Boot Script for Real Hardware: `boot.cmd`

File: `src/boot.cmd` (you later compile this to `boot.scr` with `mkimage` for U‑Boot).

```text
fatload usb 0:1 0x04000000 kernel.fit
bootm 0x04000000
```

Line‑by‑line:

- **`fatload usb 0:1 0x04000000 kernel.fit`**
  - Command for U‑Boot’s shell language.
  - `fatload`:
    - Load a file from a **FAT filesystem** into RAM.
  - `usb 0:1`:
    - Device: first USB storage device (`0`).
    - Partition: first partition (`1`).
  - `0x04000000`:
    - Destination address in RAM where the file will be loaded.
  - `kernel.fit`:
    - The file name of your FIT image.

- **`bootm 0x04000000`**
  - Tells U‑Boot to **boot an image in memory**.
  - `bootm` understands various image formats, including **FIT images**.
  - It expects a bootable image located at address `0x04000000` (where you just loaded `kernel.fit`).
  - U‑Boot then:
    - Parses the FIT image,
    - Selects the default configuration,
    - Loads the kernel and FDT to the addresses described in `kernel.its`,
    - Jumps to the kernel entry point.

So, the full hardware flow is:

1. U‑Boot runs and executes your boot script (converted from `boot.cmd` to `boot.scr`).
2. `fatload` loads `kernel.fit` from USB to 0x04000000.
3. `bootm` interprets the FIT and loads:
   - `kernel.bin` to the kernel load address (e.g. 0x00200000 physical).
   - `x1_orangepi-rv2.dtb` to 0x31000000.
4. U‑Boot jumps to the kernel entry (ultimately your `_start`).
5. Your tiny kernel executes `wfi` and `j _start` forever.

---

## 7. Putting It All Together

### 7.1 QEMU Path (No FIT)

1. You write `a.S` and `linker.ld`.
2. `make`:
   - Assembles `a.S` → `a.o`.
   - Links with `linker.ld` → `kernel.elf`.
   - Converts to `kernel.bin`.
3. `make qemu`:
   - QEMU loads `kernel.bin` and starts execution.
   - You see the repeating `wfi` + `j` instructions in the log.

### 7.2 Real Hardware Path (With FIT + Boot Script)

1. `make fit`:
   - Verifies that `src/x1_orangepi-rv2.dtb` exists.
   - Uses `kernel.its` to package `kernel.bin` + DTB into `kernel.fit`.
2. You place `kernel.fit` and the compiled `boot.scr` on a boot medium (per lab instructions).
3. On the OrangePi RV2:
   - U‑Boot executes `boot.scr` which runs the commands from `boot.cmd`.
   - `kernel.fit` is loaded and interpreted.
   - The kernel is placed in DDR and started at the configured entry address.
4. Your `_start` runs:
   - CPU sleeps via `wfi`, then jumps back in an infinite loop.

---

## 8. Typical TA‑Level Questions You Can Answer

Here are some example questions and the key points you now know:

- **Q: Where does your kernel code start executing?**
  - At the `_start` label in `a.S`, which the linker places at the **beginning of `.text`**, mapped to the address specified in `linker.ld` (e.g. `0x80200000`).

- **Q: What does the `wfi` instruction do and why do we use it?**
  - `wfi` (**Wait For Interrupt**) puts the CPU into a low‑power state until an interrupt arrives.
  - It is more power‑friendly than a busy loop and is a common way to “park” a hart when there is no work.

- **Q: Why is `.bss` special compared to `.data`?**
  - `.data`: holds **initialized** variables with non‑zero initial values; contents are stored in the binary.
  - `.bss`: holds **zero‑initialized** variables; the binary does **not** store zeros — runtime code zeros the memory region instead.

- **Q: What is the purpose of the FIT image (`kernel.fit`)?**
  - It bundles your **kernel binary** and **device tree** in a format U‑Boot understands.
  - The `.its` file (`kernel.its`) tells U‑Boot where to load these components and what their entry addresses are.

- **Q: What is `boot.cmd` / `boot.scr` doing?**
  - `boot.cmd` contains U‑Boot shell commands:
    - `fatload`: load `kernel.fit` from a USB FAT partition into RAM.
    - `bootm`: ask U‑Boot to boot that image.
  - `boot.scr` is the **compiled** version of `boot.cmd` created by `mkimage`.

- **Q: Why don’t we link against libc or use `main()`?**
  - This is **bare‑metal**:
    - No OS, no syscalls, no standard startup code.
  - We provide our own entry (`_start`) and avoid `libc` using `-nostdlib -nostartfiles -ffreestanding`.

With this understanding, you should be able to explain:
- How each file participates in the boot process.
- Why each instruction and linker directive is written that way.
- How the build system connects all the pieces from source code to running on CPU.


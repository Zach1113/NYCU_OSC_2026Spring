# =============================================================================
# Makefile — NYCU OSC2026 Lab 0
# Hardware: OrangePi RV2 (SpacemiT Ky X1, RISC-V RV64GC)
# =============================================================================
#
# Build targets:
#   make          → Build kernel.bin (QEMU-testable raw binary)
#   make fit      → Build kernel.fit (bootable FIT image for real hardware)
#   make qemu     → Run kernel.bin on QEMU (dumps disassembly)
#   make debug    → Launch QEMU in GDB debug mode (waits for connection)
#   make clean    → Remove all build artifacts
#
# Prerequisites:
#   - riscv64-unknown-elf-gcc  (cross-compiler)
#   - riscv64-unknown-elf-ld   (linker)
#   - riscv64-unknown-elf-objcopy  (ELF → binary converter)
#   - qemu-system-riscv64      (emulator)
#   - mkimage                  (from u-boot-tools, for FIT image)
#
# Install on Ubuntu/Debian:
#   sudo apt-get install gcc-riscv64-unknown-elf qemu-system-misc u-boot-tools
#
# =============================================================================

# --- Toolchain ---------------------------------------------------------------
CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump

# --- Compiler & Assembler Flags ----------------------------------------------
# -nostdlib       : Do not link standard C library (no libc in bare-metal!)
# -nostartfiles   : Do not use standard startup files (no crt0.o)
# -ffreestanding  : This code is NOT hosted by an OS
# -Wall           : Enable all compiler warnings
CFLAGS   = -Wall -nostdlib -nostartfiles -ffreestanding
ASMFLAGS =   # Add extra assembler flags here if needed later

# --- Source files ------------------------------------------------------------
SRCDIR   = src/
ASMFILES = $(wildcard $(SRCDIR)*.S)
CFILES   = $(wildcard $(SRCDIR)*.c)
OBJFILES = $(ASMFILES:.S=.o) $(CFILES:.c=.o)

# --- Linker script -----------------------------------------------------------
LDSCRIPT = $(SRCDIR)linker.ld

# =============================================================================
# Default target: build the raw binary (sufficient for QEMU testing)
# =============================================================================
.PHONY: all
all: kernel.bin

# =============================================================================
# Build pipeline:
#   .S → .o → kernel.elf → kernel.bin
# =============================================================================

# Rule: Assemble .S → .o
$(SRCDIR)%.o: $(SRCDIR)%.S
	$(CC) $(CFLAGS) $(ASMFLAGS) -c $< -o $@

# Rule: Compile .c → .o
$(SRCDIR)%.o: $(SRCDIR)%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule: Link all .o files → kernel.elf
$(SRCDIR)kernel.elf: $(LDSCRIPT) $(OBJFILES)
	$(LD) -T $(LDSCRIPT) $(OBJFILES) -o $@
	@echo "[LD]  $@"

# Rule: Strip ELF metadata → raw binary
kernel.bin: $(SRCDIR)kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "[BIN] $@ ($(shell wc -c < $@) bytes)"

# =============================================================================
# FIT image target: packages kernel.bin + DTB for real OrangePi RV2 hardware
# Requires: mkimage (u-boot-tools) and x1_orangepi-rv2.dtb in src/
# Download DTB: https://github.com/nycu-caslab/OSC2026/raw/main/uploads/x1_orangepi-rv2.dtb
# =============================================================================
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

# =============================================================================
# QEMU targets
# =============================================================================

# Run on QEMU — dumps the disassembly of instructions as they execute.
# You should see:  wfi  j 0x80200000  repeating.
.PHONY: qemu
qemu: kernel.bin
	qemu-system-riscv64 \
		-M virt \
		-kernel kernel.bin \
		-display none \
		-d in_asm \
		-nographic

# Launch QEMU in GDB debug mode:
#   -S  : Pause CPU at startup, wait for GDB to connect
#   -s  : Open GDB server on port 1234 (shorthand for -gdb tcp::1234)
# Then in another terminal run: make gdb
.PHONY: debug
debug: kernel.bin
	@echo "QEMU paused. Connect GDB in another terminal with: make gdb"
	qemu-system-riscv64 \
		-M virt \
		-kernel kernel.bin \
		-display none \
		-S -s \
		-nographic

# Connect GDB to the QEMU debug server.
# Requires: gdb-multiarch (Ubuntu) or riscv64-elf-gdb (macOS)
.PHONY: gdb
gdb: $(SRCDIR)kernel.elf
	@GDB=$$(command -v gdb-multiarch || command -v riscv64-elf-gdb || command -v riscv64-unknown-elf-gdb); \
	if [ -z "$$GDB" ]; then \
		echo "ERROR: no suitable GDB found (tried gdb-multiarch, riscv64-elf-gdb, riscv64-unknown-elf-gdb)"; \
		exit 1; \
	fi; \
	echo "Using GDB: $$GDB"; \
	$$GDB -ex "file $(SRCDIR)kernel.elf" \
	      -ex "target remote :1234" \
	      -ex "layout asm"

# Build with debug symbols (-g) for use with GDB
.PHONY: debug-build
debug-build: CFLAGS += -g
debug-build: ASMFLAGS += -g
debug-build: kernel.bin

# =============================================================================
# Utility targets
# =============================================================================

# Disassemble the ELF for inspection (without running)
.PHONY: disasm
disasm: $(SRCDIR)kernel.elf
	$(OBJDUMP) -d $<

# Clean all build artifacts
.PHONY: clean
clean:
	rm -f $(SRCDIR)*.o $(SRCDIR)*.elf $(SRCDIR)kernel.bin
	rm -f kernel.bin kernel.fit
	@echo "[CLEAN] Done"

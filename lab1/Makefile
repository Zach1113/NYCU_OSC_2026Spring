# =============================================================================
# Makefile — NYCU OSC2026 Lab 1
# =============================================================================

CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

CFLAGS   = -Wall -mcmodel=medany -ffreestanding -nostdlib -nostartfiles -g
QEMU     = qemu-system-riscv64

SRCDIR   = src/
LDSCRIPT = $(SRCDIR)linker.ld

ASMFILES = $(wildcard $(SRCDIR)*.S)
CFILES   = $(wildcard $(SRCDIR)*.c)
OBJFILES = $(ASMFILES:.S=.o) $(CFILES:.c=.o)

# --- Build ---
.PHONY: all
all: kernel.bin

$(SRCDIR)%.o: $(SRCDIR)%.S
	$(CC) $(CFLAGS) -c $< -o $@

$(SRCDIR)%.o: $(SRCDIR)%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(SRCDIR)kernel.elf: $(LDSCRIPT) $(OBJFILES)
	$(LD) -T $(LDSCRIPT) $(OBJFILES) -o $@

kernel.bin: $(SRCDIR)kernel.elf
	$(OBJCOPY) -O binary $< $@

# --- FIT image (OrangePi RV2 SD card) ---
.PHONY: fit
fit: kernel.bin
	@if [ ! -f $(SRCDIR)x1_orangepi-rv2.dtb ]; then \
		echo ""; \
		echo "ERROR: $(SRCDIR)x1_orangepi-rv2.dtb not found!"; \
		echo "Copy it from lab0/src/ or download from the course repo."; \
		echo ""; \
		exit 1; \
	fi
	cp kernel.bin $(SRCDIR)kernel.bin
	mkimage -f $(SRCDIR)kernel.its kernel.fit
	rm -f $(SRCDIR)kernel.bin
	@echo "[FIT] kernel.fit ready — copy to SD card boot partition"

# --- QEMU ---
.PHONY: run
run: kernel.bin
	$(QEMU) -M virt -m 128M -kernel kernel.bin -nographic -monitor none

.PHONY: debug
debug: kernel.bin
	$(QEMU) -M virt -m 128M -kernel kernel.bin -nographic -monitor none -S -s

# --- Clean ---
.PHONY: clean
clean:
	rm -f $(SRCDIR)*.o $(SRCDIR)*.elf
	rm -f kernel.bin kernel.fit boot.scr

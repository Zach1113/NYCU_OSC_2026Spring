# =============================================================================
# Makefile — NYCU OSC2026 Lab 1
# =============================================================================

CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

CFLAGS   = -Wall -mcmodel=medany -ffreestanding -nostdlib -nostartfiles -g
QEMU     = qemu-system-riscv64
PYTHON   = python3

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
	@if [ ! -f initramfs.cpio ]; then \
		echo ""; \
		echo "ERROR: initramfs.cpio not found in lab2/"; \
		echo "Create one from rootfs then run make fit again."; \
		echo ""; \
		exit 1; \
	fi
	cp kernel.bin $(SRCDIR)kernel.bin
	cp initramfs.cpio $(SRCDIR)initramfs.cpio
	mkimage -f $(SRCDIR)kernel.its kernel.fit
	rm -f $(SRCDIR)kernel.bin
	rm -f $(SRCDIR)initramfs.cpio
	@echo "[FIT] kernel.fit ready — copy to SD card boot partition"

# --- QEMU ---
.PHONY: run
run: kernel.bin
	$(QEMU) -M virt -kernel kernel.bin -display none -nographic

.PHONY: run-initrd
run-initrd: kernel.bin initramfs.cpio
	$(QEMU) -M virt -kernel kernel.bin -initrd initramfs.cpio -display none -nographic

.PHONY: debug
debug: kernel.bin
	$(QEMU) -M virt -kernel kernel.bin -display none -nographic -S -s

# --- UART load helper ---
PORT ?= /dev/ttyUSB0

.PHONY: send
send: kernel.bin
	$(PYTHON) scripts/send_kernel.py $(PORT) kernel.bin

# --- Clean ---
.PHONY: clean
clean:
	rm -f $(SRCDIR)*.o $(SRCDIR)*.elf
	rm -f kernel.bin kernel.fit boot.scr

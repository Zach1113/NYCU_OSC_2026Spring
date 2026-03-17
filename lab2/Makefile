# =============================================================================
# Makefile — NYCU OSC2026 Lab 1
# =============================================================================

CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

CFLAGS   = -Wall -mcmodel=medany -ffreestanding -nostdlib -nostartfiles -g \
		   -march=rv64imac_zicsr_zifencei -mabi=lp64
QEMU     = qemu-system-riscv64
PYTHON   = python3

SRCDIR   = src/

LDSCRIPT_BOOT    = $(SRCDIR)linker.ld
LDSCRIPT_PAYLOAD = $(SRCDIR)linker_payload.ld

BOOT_ELF     = $(SRCDIR)kernel_boot.elf
BOOT_BIN     = kernel_boot.bin
PAYLOAD_ELF  = $(SRCDIR)kernel_payload.elf
PAYLOAD_BIN  = kernel_payload.bin

ASMFILES = $(wildcard $(SRCDIR)*.S)
CFILES   = $(wildcard $(SRCDIR)*.c)
OBJFILES = $(ASMFILES:.S=.o) $(CFILES:.c=.o)

# --- Build ---
.PHONY: all
all: $(BOOT_BIN) $(PAYLOAD_BIN) kernel.bin

$(SRCDIR)%.o: $(SRCDIR)%.S
	$(CC) $(CFLAGS) -c $< -o $@

$(SRCDIR)%.o: $(SRCDIR)%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_ELF): $(LDSCRIPT_BOOT) $(OBJFILES)
	$(LD) -T $(LDSCRIPT_BOOT) $(OBJFILES) -o $@

$(BOOT_BIN): $(BOOT_ELF)
	$(OBJCOPY) -O binary $< $@

$(PAYLOAD_ELF): $(LDSCRIPT_PAYLOAD) $(OBJFILES)
	$(LD) -T $(LDSCRIPT_PAYLOAD) $(OBJFILES) -o $@

$(PAYLOAD_BIN): $(PAYLOAD_ELF)
	$(OBJCOPY) -O binary $< $@

# Keep compatibility with existing workflows expecting kernel.bin.
kernel.bin: $(BOOT_BIN)
	cp $(BOOT_BIN) kernel.bin

# --- FIT image (OrangePi RV2 SD card) ---
.PHONY: fit
fit: $(BOOT_BIN)
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
	cp $(BOOT_BIN) $(SRCDIR)kernel.bin
	cp initramfs.cpio $(SRCDIR)initramfs.cpio
	mkimage -f $(SRCDIR)kernel.its kernel.fit
	rm -f $(SRCDIR)kernel.bin
	rm -f $(SRCDIR)initramfs.cpio
	@echo "[FIT] kernel.fit ready — copy to SD card boot partition"

# --- QEMU ---
.PHONY: run
run: $(BOOT_BIN)
	$(QEMU) -M virt -kernel $(BOOT_BIN) -display none -nographic

.PHONY: run-initrd
run-initrd: $(BOOT_BIN) initramfs.cpio
	$(QEMU) -M virt -kernel $(BOOT_BIN) -initrd initramfs.cpio -display none -nographic

.PHONY: debug
debug: $(BOOT_BIN)
	$(QEMU) -M virt -kernel $(BOOT_BIN) -display none -nographic -S -s

# --- UART load helper ---
PORT ?= /dev/ttyUSB0

.PHONY: send
send: $(PAYLOAD_BIN)
	$(PYTHON) scripts/send_kernel.py $(PORT) $(PAYLOAD_BIN)

# --- Clean ---
.PHONY: clean
clean:
	rm -f $(SRCDIR)*.o $(SRCDIR)*.elf
	rm -f kernel.bin $(BOOT_BIN) $(PAYLOAD_BIN) kernel.fit boot.scr

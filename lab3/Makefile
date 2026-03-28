# =============================================================================
# Makefile — NYCU OSC2026 Lab 2
# =============================================================================

CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

CFLAGS   = -Wall -mcmodel=medany -ffreestanding -nostdlib -nostartfiles -g \
		   -march=rv64imac_zicsr_zifencei -mabi=lp64
CPPFLAGS = -I$(SRCDIR)
QEMU     = qemu-system-riscv64
PYTHON   = python3

SRCDIR   = src
BOOTDIR  = $(SRCDIR)/bootloader
KERNELDIR = $(SRCDIR)/kernel

LDSCRIPT_BOOT    = $(BOOTDIR)/linker.ld
LDSCRIPT_KERNEL  = $(KERNELDIR)/linker.ld

BOOT_ELF     = $(BOOTDIR)/boot.elf
BOOT_BIN     = boot.bin
BOOT_FIT     = kernel.fit
KERNEL_ELF   = $(KERNELDIR)/kernel.elf
KERNEL_BIN   = kernel.bin

BOOT_ASMFILES = $(wildcard $(BOOTDIR)/*.S)
BOOT_CFILES   = $(wildcard $(BOOTDIR)/*.c)
BOOT_SHARED_CFILES = $(SRCDIR)/uart.c $(SRCDIR)/fdt.c
BOOT_OBJFILES = $(BOOT_ASMFILES:.S=.o) \
                $(BOOT_CFILES:.c=.o) \
                $(BOOT_SHARED_CFILES:.c=.o)

KERNEL_ASMFILES = $(wildcard $(KERNELDIR)/*.S)
KERNEL_CFILES   = $(wildcard $(KERNELDIR)/*.c)
KERNEL_SHARED_CFILES = $(SRCDIR)/uart.c $(SRCDIR)/fdt.c $(SRCDIR)/cpio.c $(SRCDIR)/mm.c
KERNEL_OBJFILES = $(KERNEL_ASMFILES:.S=.o) \
                  $(KERNEL_CFILES:.c=.o) \
                  $(KERNEL_SHARED_CFILES:.c=.o)

# --- Build ---
.PHONY: all build
all: $(BOOT_BIN) $(KERNEL_BIN)
build: $(BOOT_BIN) $(KERNEL_BIN)

$(BOOTDIR)/%.o: $(BOOTDIR)/%.S
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BOOTDIR)/%.o: $(BOOTDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(KERNELDIR)/%.o: $(KERNELDIR)/%.S
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(KERNELDIR)/%.o: $(KERNELDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BOOT_ELF): $(LDSCRIPT_BOOT) $(BOOT_OBJFILES)
	$(LD) -T $(LDSCRIPT_BOOT) $(BOOT_OBJFILES) -o $@

$(BOOT_BIN): $(BOOT_ELF)
	$(OBJCOPY) -O binary $< $@

$(KERNEL_ELF): $(LDSCRIPT_KERNEL) $(KERNEL_OBJFILES)
	$(LD) -T $(LDSCRIPT_KERNEL) $(KERNEL_OBJFILES) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# --- FIT image (OrangePi RV2 SD card) ---
.PHONY: fit
fit: $(BOOT_BIN)
	@if [ ! -f $(SRCDIR)/x1_orangepi-rv2.dtb ]; then \
		echo ""; \
		echo "ERROR: $(SRCDIR)/x1_orangepi-rv2.dtb not found!"; \
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
	cp $(BOOT_BIN) $(SRCDIR)/boot.bin
	cp initramfs.cpio $(SRCDIR)/initramfs.cpio
	mkimage -f $(SRCDIR)/kernel.its $(BOOT_FIT)
	rm -f $(SRCDIR)/boot.bin
	rm -f $(SRCDIR)/initramfs.cpio
	@echo "[FIT] $(BOOT_FIT) ready — copy to SD card boot partition"

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
send: $(KERNEL_BIN)
	$(PYTHON) send_kernel.py $(PORT) $(KERNEL_BIN)

# Or run sudo python3 send_kernel.py /dev/ttyUSB0 kernel.bin

# --- Clean ---
.PHONY: clean
clean:
	rm -f $(BOOTDIR)/*.o $(BOOTDIR)/*.elf
	rm -f $(SRCDIR)/*.o $(KERNELDIR)/*.o $(KERNELDIR)/*.elf
	rm -f $(KERNEL_BIN) $(BOOT_BIN) $(BOOT_FIT) boot.scr
	rm -f $(SRCDIR)/boot.bin $(SRCDIR)/initramfs.cpio
	rm -f kernel_boot.bin boot.fit $(BOOTDIR)/kernel_boot.elf

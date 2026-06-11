# =============================================================================
# Makefile - NYCU OSC2026 Lab 5
# =============================================================================

CROSS    = riscv64-unknown-elf-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

CFLAGS   = -Wall -mcmodel=medany -ffreestanding -nostdlib -nostartfiles -g \
		   -march=rv64imac_zicsr_zifencei -mabi=lp64
CPPFLAGS = -I$(INCDIR) -I$(SRCDIR)
QEMU     = qemu-system-riscv64
PYTHON   = python3

SRCDIR   = src
INCDIR   = include
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
KERNEL_SHARED_CFILES = $(SRCDIR)/uart.c $(SRCDIR)/fdt.c $(SRCDIR)/cpio.c $(SRCDIR)/mm.c $(SRCDIR)/vm.c \
			      $(SRCDIR)/timer.c $(SRCDIR)/plic.c $(SRCDIR)/task.c \
			      $(SRCDIR)/sched.c $(SRCDIR)/user_vm.c $(SRCDIR)/syscall.c $(SRCDIR)/mmap.c $(SRCDIR)/video.c \
                  $(SRCDIR)/vfs.c $(SRCDIR)/tmpfs.c $(SRCDIR)/ramfs.c $(SRCDIR)/devfs.c
KERNEL_OBJFILES = $(KERNEL_ASMFILES:.S=.o) \
                  $(KERNEL_CFILES:.c=.o) \
                  $(KERNEL_SHARED_CFILES:.c=.o)

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

.PHONY: fit
fit: $(KERNEL_BIN)
	@if [ ! -f $(SRCDIR)/x1_orangepi-rv2.dtb ]; then \
		echo "ERROR: $(SRCDIR)/x1_orangepi-rv2.dtb not found!"; \
		exit 1; \
	fi
	@if [ ! -f initramfs.cpio ]; then \
		echo "ERROR: initramfs.cpio not found in lab5/"; \
		exit 1; \
	fi
	cp $(KERNEL_BIN) $(SRCDIR)/kernel.bin
	cp initramfs.cpio $(SRCDIR)/initramfs.cpio
	mkimage -f $(SRCDIR)/kernel.its $(BOOT_FIT)
	rm -f $(SRCDIR)/kernel.bin
	rm -f $(SRCDIR)/initramfs.cpio
	@echo "[FIT] $(BOOT_FIT) ready - copy to SD card boot partition"

.PHONY: run
run: $(BOOT_BIN)
	$(QEMU) -M virt -kernel $(BOOT_BIN) -display none -nographic

.PHONY: run-initrd
run-initrd: $(BOOT_BIN) initramfs.cpio
	$(QEMU) -M virt -kernel $(BOOT_BIN) -initrd initramfs.cpio -display none -nographic

.PHONY: debug
debug: $(BOOT_BIN)
	$(QEMU) -M virt -kernel $(BOOT_BIN) -display none -nographic -S -s

PORT ?= /dev/ttyUSB0

.PHONY: send
send: $(KERNEL_BIN)
	$(PYTHON) send_kernel.py $(PORT) $(KERNEL_BIN)

.PHONY: clean
clean:
	rm -f $(BOOTDIR)/*.o $(BOOTDIR)/*.elf
	rm -f $(SRCDIR)/*.o $(KERNELDIR)/*.o $(KERNELDIR)/*.elf
	rm -f $(KERNEL_BIN) $(BOOT_BIN) $(BOOT_FIT) boot.scr
	rm -f $(SRCDIR)/boot.bin $(SRCDIR)/kernel.bin $(SRCDIR)/initramfs.cpio

# OSC 2026 | Lab 0: Environment Setup

## Hardware Platform
**OrangePi RV2** — SpacemiT Ky X1 SoC, octa-core 64-bit RISC-V (RV64GC), up to 1.6 GHz

## Source Files

| File | Purpose |
|------|---------|
| `src/a.S` | Minimal RISC-V assembly kernel (WFI infinite loop) |
| `src/linker.ld` | Linker script — places code at `0x80200000` |
| `src/kernel.its` | FIT image descriptor (kernel + device tree) |
| `Makefile` | Build automation |

## Build Pipeline

```
src/a.S
   │  [riscv64-unknown-elf-gcc -c]
   ▼
src/a.o
   │  [riscv64-unknown-elf-ld -T src/linker.ld]
   ▼
src/kernel.elf
   │  [riscv64-unknown-elf-objcopy -O binary]
   ▼
kernel.bin          ← QEMU-testable raw binary
   │  [mkimage -f src/kernel.its]  (requires x1_orangepi-rv2.dtb)
   ▼
kernel.fit          ← Bootable on real OrangePi RV2 hardware
```

## Prerequisites

### Install Cross-Compiler
```bash
# Ubuntu/Debian
sudo apt-get install gcc-riscv64-unknown-elf

# macOS (Homebrew)
brew tap riscv-software-src/riscv && brew install riscv-tools
```

### Install QEMU
```bash
# Ubuntu/Debian
sudo apt-get install qemu-system-misc

# macOS
brew install qemu
```

### Install u-boot-tools (for FIT image)
```bash
# Ubuntu/Debian
sudo apt-get install u-boot-tools

# macOS
brew install u-boot-tools
```

### Download Device Tree Blob (for real hardware)
```bash
wget https://github.com/nycu-caslab/OSC2026/raw/main/uploads/x1_orangepi-rv2.dtb \
     -O src/x1_orangepi-rv2.dtb
```

## Usage

```bash
# Build raw binary (for QEMU)
make

# Test on QEMU — dumps disassembly, Ctrl-C to exit
make qemu

# Build FIT image for OrangePi RV2 (requires DTB in src/)
make fit

# Launch QEMU in GDB debug mode
make debug       # Terminal 1
make gdb         # Terminal 2

# View disassembly without running
make disasm

# Clean build artifacts
make clean
```

## Expected QEMU Output

```
IN:
0x80200000:  10500073  wfi
0x80200004:  ffdff06f  j  0x80200000
```

This confirms the kernel loaded at `0x80200000` and is executing correctly.

## References
- [Course Website (OSC2026)](https://nycu-caslab.github.io/OSC2026/)
- [Lab 0 Spec](https://nycu-caslab.github.io/OSC2026/labs/lab0.html)
- [SpacemiT K1 User Manual](https://github.com/nycu-caslab/OSC2026/raw/main/references/K1_User_Manual_(V6.1_2025.08.06).pdf)
- [OrangePi RV2 Official Wiki](http://www.orangepi.org/orangepiwiki/index.php/Orange_Pi_RV2)

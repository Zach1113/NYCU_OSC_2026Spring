# NYCU Operating System Capstone 2026 Spring

## Course overview
This course aims to introduce the design and implementation of operating system kernels. You’ll learn both concept and implementation from a series of labs.

This course uses [OrangePi RV2](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-RV2.html) as the hardware platform. Students can get their hands dirty on a Real Machine instead of an emulator.

---

## Lab 0: Environment Setup

### Introduction
In Lab 0, you will prepare the development environment for the future labs. You should install the required toolchain and use it to build a bootable image for OrangePi RV2.

### Goals
- Install the RISC-V toolchain and emulator on your host system.
- Learn the fundamentals of cross-platform bare-metal development.
- Build and boot a minimal kernel image on QEMU and OrangePi RV2.
---

## Lab 1: Hello World

## Introduction
In this lab, you will begin practicing bare-metal programming on the OrangePi RV2 board by implementing a minimal interactive shell.

This lab focuses on configuring the Universal Asynchronous Receiver-Transmitter (UART) interface for serial communication, which serves as the primary I/O channel between your host computer and the OrangePi RV2 during development. You will also gain experience in low-level system initialization, peripheral access, and basic input/output handling.

### Goals
- Practice bare-metal programming.
- Understand how to access OrangePi RV2’s peripherals.
- Set up UART for serial communication.
---

## Lab 2: Booting

### Introduction
Booting is the process of initializing the system environment to run various user programs after a computer reset. This includes loading the kernel, initializing subsystems, matching device drivers, and launching the initial user program to bring up remaining services in user space.

In Lab 2, you’ll implement a bootloader for the OrangePi RV2 that loads kernel images through UART. Additionally, you’ll gain an understanding of devicetrees and initial ramdisk.

### Goals
- Implement a bootloader that loads kernel images through UART.
- Understand the structure and purpose of devicetrees.
- Understand the concept and usage of initial ramdisk.
---

## Lab 3: Memory Allocator

### Introduction

### Goals
---

## Lab 4: Exception and Interrupt

### Introduction

### Goals
---

## Lab 5: Thread and User Process

### Introduction

### Goals
---
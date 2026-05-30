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
A kernel allocates physical memory for maintaining its internal states and user programs’ use. Without memory allocators, you need to statically partition the physical memory into several memory pools for different objects. It’s sufficient for some systems that run known applications on known devices. Yet, general-purpose operating systems that run diverse applications on diverse devices determine the use and amount of physical memory at runtime. On OrangePi RV2, available physical memory and reserved regions are described by the Device Tree, so you must read those descriptions at boot.

In Lab 3, you need to implement memory allocators that rely on OrangePi RV2’s Device Tree for memory layout. They’ll be used in all later labs.

### Goals
- Implement a Page Frame Allocator.
- Implement a Dynamic Memory Allocator that requests pages from the previous Page Frame Allocator and partitions pages into chunk pools.
- Implement a Startup Allocator (bump-pointer).
---

## Lab 4: Exception and Interrupt

### Introduction
An exception is an event that causes the currently executing program to relinquish the CPU to the corresponding handler. With the exception mechanism, an operating system can

1. handle errors properly during execution,
2. allow user programs to request system services,
3. respond to peripheral devices that require immediate attention.


### Goals
- Understand the exception mechanism in RISC-V.
- Understand how interrupt delegation works in the OrangePi RV2 platform.
- Configure and handle core timer interrupts using the SBI Timer Extension.
- Understand and handle UART interrupts via the PLIC.
- Learn how to multiplex timers and schedule asynchronous tasks.
---

## Lab 5: Thread and User Process

### Introduction
Multitasking is the most important feature of an operating system. In this lab, you’ll learn how to create threads and how to switch between different threads to achieve multitasking. Moreover, you’ll learn how a user program becomes a user process and accesses services provided by the kernel through system calls.

### Goals
- Understand how to create threads and user processes.
- Understand how to implement scheduler and context switch.
- Understand what’s preemption.
- Understand how to implement POSIX signals.
---

## Lab 6: Virtual Memory

### Introduction
Virtual memory provides isolated address spaces, so each user process can run in its address space without interfering with others.

In this lab, you need to initialize the memory management unit (MMU) and set up the address spaces for the kernel and user processes to achieve process isolation.

### Goals
- Understand RISC-V Sv39 virtual memory system architecture.
- Understand how the kernel manages memory for user processes.
- Understand how demand paging works.
- Understand how copy-on-write works.
---

## Lab 7: Virtual File System

### Introduction
A file system manages data in storage mediums. Each file system has a specific way to store and retrieve the data. Hence, a virtual file system (VFS) is common in general-purpose OS, providing a unified interface for all file systems.

In this lab, you’ll implement a VFS interface for your kernel, and a memory-based file system (tmpfs) that mounts as the root file system, you’ll also implement special file for uart and framebuffer.

### Goals
- Understand how VFS interface works.
- Understand how to set up a root file system.
- Understand how to operate on files.
- Understand how to mount a file system and look up a file across file systems.
- Understand how special file works.
#ifndef SYSCALL_H
#define SYSCALL_H

#include "trap.h"

void syscall_handle(struct trapframe *tf);
void signal_deliver(struct trapframe *tf);

#endif

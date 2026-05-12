#include "trap.h"

#include "plic.h"
#include "sched.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"
#include "uart.h"

#define SCAUSE_INTERRUPT_BIT (1UL << 63)
#define SCAUSE_ECALL_UMODE   8UL
#define SCAUSE_STIMER        5UL
#define SCAUSE_SEXTERNAL     9UL

extern void kernel_trap_vector(void);

static struct {
    unsigned long scause;
    unsigned long sepc;
    unsigned long stval;
} g_trap_report;

static void write_stvec(void *fn) {
    asm volatile("csrw stvec, %0" : : "r"(fn));
}

static void write_sscratch(unsigned long v) {
    asm volatile("csrw sscratch, %0" : : "r"(v));
}

static void print_trap_info(unsigned long scause,
                            unsigned long sepc,
                            unsigned long stval) {
    uart_puts("=== S-Mode trap ===\n");
    uart_puts("scause: ");
    uart_dec(scause);
    uart_putc('\n');
    uart_puts("sepc: ");
    uart_hex(sepc);
    uart_putc('\n');
    uart_puts("stval: ");
    uart_dec(stval);
    uart_putc('\n');
}

static void print_trap_info_task(void *arg) {
    (void)arg;
    print_trap_info(g_trap_report.scause,
                    g_trap_report.sepc,
                    g_trap_report.stval);
}

void trap_init(void) {
    write_stvec(kernel_trap_vector);
    write_sscratch(0);
}

void trap_handler(struct trapframe *tf) {
    unsigned long scause = tf->scause;
    unsigned long stval = tf->stval;
    unsigned long cause = scause & ~SCAUSE_INTERRUPT_BIT;

    if (scause & SCAUSE_INTERRUPT_BIT) {
        if (cause == SCAUSE_STIMER) {
            timer_handle_interrupt();
            scheduler_wake_sleepers(timer_now());
            task_run_ready();
            schedule();
            return;
        }

        if (cause == SCAUSE_SEXTERNAL) {
            int irq;

            while ((irq = plic_claim()) != 0) {
                if (irq == uart_irq_number())
                    uart_irq_handler();
                plic_complete(irq);
            }
            task_run_ready();
            schedule();
            return;
        }
    }

    if ((scause & SCAUSE_INTERRUPT_BIT) == 0 && cause == SCAUSE_ECALL_UMODE) {
        syscall_handle(tf);
        task_run_ready();
        return;
    }

    g_trap_report.scause = scause;
    g_trap_report.sepc = tf->sepc;
    g_trap_report.stval = stval;
    if (!add_task(print_trap_info_task, 0, 64))
        print_trap_info(scause, tf->sepc, stval);

    task_run_ready();
    if (get_current() && get_current()->is_user) {
        uart_puts("[Trap] Killing current user process.\n");
        thread_exit();
    }

    uart_puts("[Trap] Unhandled kernel trap. Halting.\n");
    while (1) {
    }
}

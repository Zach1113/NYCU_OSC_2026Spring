#include "trap.h"

#include "plic.h"
#include "task.h"
#include "timer.h"
#include "uart.h"

#define SSTATUS_SPP   (1UL << 8) // Supervisor Previous Privilege, 0 for User mode, 1 for Supervisor mode
#define SSTATUS_SPIE  (1UL << 5) // Supervisor Previous Interrupt Enable, 0 for interrupts disabled, 1 for interrupts enabled

#define SCAUSE_INTERRUPT_BIT (1UL << 63)
#define SCAUSE_ECALL_UMODE   8UL
#define SCAUSE_STIMER        5UL
#define SCAUSE_SEXTERNAL     9UL

struct kernel_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long gp;
    unsigned long tp;
    unsigned long s0;
    unsigned long s1;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
};

extern void kernel_trap_vector(void);
extern int enter_user_mode(struct trapframe *tf,
                           struct kernel_context *ctx,
                           unsigned long kernel_trap_sp);

static struct kernel_context g_kernel_ctx;
static struct trapframe g_user_tf;
static unsigned char g_user_trap_stack[4096] __attribute__((aligned(16)));
static struct {
    unsigned long scause;
    unsigned long sepc;
    unsigned long stval;
} g_trap_report;

static unsigned long read_sstatus(void) {
    unsigned long v;
    asm volatile("csrr %0, sstatus" : "=r"(v));
    return v;
}

static unsigned long read_scause(void) {
    unsigned long v;
    asm volatile("csrr %0, scause" : "=r"(v));
    return v;
}

static unsigned long read_stval(void) {
    unsigned long v;
    asm volatile("csrr %0, stval" : "=r"(v));
    return v;
}

static void write_stvec(void *fn) {
    asm volatile("csrw stvec, %0" : : "r"(fn));
}

static void write_sscratch(unsigned long v) {
    asm volatile("csrw sscratch, %0" : : "r"(v));
}

static void zero_bytes(void *ptr, unsigned long len) {
    unsigned char *p = (unsigned char *)ptr;
    while (len--)
        *p++ = 0;
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

int run_user_program(unsigned long entry, unsigned long user_sp) {
    unsigned long sstatus;

    zero_bytes(&g_user_tf, sizeof(g_user_tf));
    sstatus = read_sstatus();
    sstatus &= ~SSTATUS_SPP; // set SPP to 0 for User mode
    sstatus |= SSTATUS_SPIE; // enable interrupts in User mode

    g_user_tf.sp = user_sp;
    g_user_tf.sepc = entry;
    g_user_tf.sstatus = sstatus;

    return enter_user_mode(&g_user_tf,
                           &g_kernel_ctx,
                           (unsigned long)(g_user_trap_stack +
                                           sizeof(g_user_trap_stack)));
}

void trap_handler(struct trapframe *tf) {
    unsigned long scause = read_scause();
    unsigned long stval = read_stval();
    unsigned long cause = scause & ~SCAUSE_INTERRUPT_BIT;

    if (scause & SCAUSE_INTERRUPT_BIT) {
        if (cause == SCAUSE_STIMER) {
            timer_handle_interrupt();
            task_run_ready();
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
            return;
        }
    }

    g_trap_report.scause = scause;
    g_trap_report.sepc = tf->sepc;
    g_trap_report.stval = stval;
    if (!add_task(print_trap_info_task, 0, 64))
        print_trap_info(scause, tf->sepc, stval);

    if ((scause & SCAUSE_INTERRUPT_BIT) == 0 && cause == SCAUSE_ECALL_UMODE) {
        tf->sepc += 4;
        task_run_ready();
        return;
    }

    task_run_ready();
    uart_puts("[Trap] Unhandled trap. Halting.\n");
    while (1)
        ;
}

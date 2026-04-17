#include "fdt.h"
#include "cpio.h"
#include "uart.h"
#include "mm.h"
#include "plic.h"
#include "timer.h"
#include "trap.h"
#include "task.h"

#define SBI_EXT_BASE  0x10
#define USER_STACK_SIZE 4096UL
#define SIE_SEIE (1UL << 9)
#define SSTATUS_SIE (1UL << 1)

#define SBI_EXT_BASE_GET_SPEC_VERSION   0
#define SBI_EXT_BASE_GET_IMP_ID         1
#define SBI_EXT_BASE_GET_IMP_VERSION    2

struct sbiret {
    long error;
    long value;
};

void start_kernel(unsigned long hartid, const void *fdt);

/*
 *   a7 = extension ID
 *   a6 = function ID
 *   a0-a5 = arguments
 *   Returns: a0 = error, a1 = value
 */
struct sbiret sbi_ecall(int ext, int fid,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5) {
    struct sbiret ret;
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = (unsigned long)fid;
    register unsigned long a7 asm("a7") = (unsigned long)ext;
    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");
    ret.error = a0;
    ret.value = a1;
    return ret;
}

/* strcmp without libc */
static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static unsigned long str_len(const char *s) {
    unsigned long n = 0;

    while (s && s[n])
        n++;
    return n;
}

static void copy_bytes(char *dst, const char *src, unsigned long n) {
    unsigned long i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

struct timeout_request {
    unsigned long command_time_sec;
    char *message;
};

static void timeout_print_cb(void *arg) {
    struct timeout_request *req = (struct timeout_request *)arg;

    if (!req)
        return;

    uart_puts("[setTimeout] now=");
    uart_dec(timer_seconds_since_boot());
    uart_puts(" cmd=");
    uart_dec(req->command_time_sec);
    uart_puts(" msg=");
    uart_puts(req->message ? req->message : "(null)");
    uart_putc('\n');

    if (req->message)
        free(req->message);
    free(req);
}

static void enable_supervisor_external_interrupts(void) {
    unsigned long v;

    asm volatile("csrr %0, sie" : "=r"(v));
    v |= SIE_SEIE;
    asm volatile("csrw sie, %0" : : "r"(v));

    asm volatile("csrr %0, sstatus" : "=r"(v));
    v |= SSTATUS_SIE;
    asm volatile("csrw sstatus, %0" : : "r"(v));
}

/* Shell commands */
static void cmd_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("  help   - Show this help message\n");
    uart_puts("  hello  - Print Hello World!\n");
    uart_puts("  info   - Show OpenSBI info\n");
    uart_puts("  ls     - List files in initramfs cpio\n");
    uart_puts("  cat    - Print file content from initramfs (usage: cat <name>)\n");
    uart_puts("  exec   - Run a user program from initramfs (usage: exec [name])\n");
    uart_puts("  mtest  - Run memory allocator self-test\n");
    uart_puts("  mlog   - Set allocator verbose log (usage: mlog on|off)\n");
    uart_puts("  heartbeat - Toggle 2-second timer print (usage: heartbeat on|off)\n");
    uart_puts("  setTimeout - Print a message later (usage: setTimeout <sec> <msg>)\n");
    uart_puts("  taskbatch - Queue spec-style demo tasks with priorities 1/3/5\n");
}

static void cmd_hello(void) {
    uart_puts("Hello World!\n");
}

static void cmd_info(void) {
    struct sbiret r;

    r = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_SPEC_VERSION, 0,0,0,0,0,0);
    uart_puts("SBI spec version     : ");
    uart_hex(r.value);
    uart_putc('\n');

    r = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMP_ID, 0,0,0,0,0,0);
    uart_puts("SBI implementation ID: ");
    uart_hex(r.value);
    uart_putc('\n');

    r = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMP_VERSION, 0,0,0,0,0,0);
    uart_puts("SBI impl. version    : ");
    uart_hex(r.value);
    uart_putc('\n');
}

static void cmd_ls(void) {
    cpio_ls();
}

static void cmd_cat(const char *name) {
    cpio_cat(name);
}

static void cmd_mtest(void) {
    mm_self_test();
}

static void cmd_mlog(const char *arg) {
    if (!arg || streq(arg, "on")) {
        mm_set_log_enabled(1);
        return;
    }
    if (streq(arg, "off")) {
        mm_set_log_enabled(0);
        return;
    }
    uart_puts("Usage: mlog on|off\n");
}

static void cmd_heartbeat(const char *arg) {
    if (!arg || streq(arg, "on")) {
        timer_set_periodic_enabled(1);
        uart_puts("heartbeat: on\n");
        return;
    }
    if (streq(arg, "off")) {
        timer_set_periodic_enabled(0);
        uart_puts("heartbeat: off\n");
        return;
    }
    uart_puts("Usage: heartbeat on|off\n");
}

static void cmd_exec(const char *name) {
    const void *prog = 0;
    unsigned long prog_size = 0;
    static void *user_stack;
    const char *prog_name = name && *name ? name : "prog.bin";
    unsigned long user_sp;
    int ret;

    if (!cpio_find(prog_name, &prog, &prog_size)) {
        uart_puts("exec: file not found: ");
        uart_puts(prog_name);
        uart_putc('\n');
        return;
    }

    if (!user_stack) {
        user_stack = alloc(USER_STACK_SIZE);
        if (!user_stack) {
            uart_puts("exec: failed to allocate user stack\n");
            return;
        }
    }

    user_sp = (unsigned long)user_stack + USER_STACK_SIZE;

    uart_puts("exec: entering U-mode at ");
    uart_hex((unsigned long)prog);
    uart_puts(" size=");
    uart_hex(prog_size);
    uart_putc('\n');

    ret = run_user_program((unsigned long)prog, user_sp);
    uart_puts("exec: user program returned with code ");
    uart_dec((unsigned long)ret);
    uart_putc('\n');
}

static void cmd_set_timeout(const char *arg) {
    char *msg;
    char *msg_copy;
    struct timeout_request *req;
    unsigned long sec = 0;
    const char *p = arg;

    while (*p == ' ')
        p++;
    if (*p < '0' || *p > '9') {
        uart_puts("Usage: setTimeout <sec> <msg>\n");
        return;
    }
    while (*p >= '0' && *p <= '9') {
        sec = sec * 10UL + (unsigned long)(*p - '0');
        p++;
    }
    while (*p == ' ')
        p++;
    if (*p == '\0') {
        uart_puts("Usage: setTimeout <sec> <msg>\n");
        return;
    }

    msg = (char *)p;
    msg_copy = (char *)alloc(str_len(msg) + 1UL);
    req = (struct timeout_request *)alloc(sizeof(*req));
    if (!msg_copy || !req) {
        uart_puts("setTimeout: allocation failed\n");
        if (msg_copy)
            free(msg_copy);
        if (req)
            free(req);
        return;
    }

    copy_bytes(msg_copy, msg, str_len(msg) + 1UL);
    req->command_time_sec = timer_seconds_since_boot();
    req->message = msg_copy;

    if (!add_timer(timeout_print_cb, req, (int)sec)) {
        uart_puts("setTimeout: timer queue full\n");
        free(msg_copy);
        free(req);
    }
}

static void cmd_taskbatch(void) {
    if (!task_enqueue_demo_batch()) {
        uart_puts("taskbatch: task queue full\n");
    }
}

static void shell_execute_command(char *buf) {
    if (streq(buf, "help"))
        cmd_help();
    else if (streq(buf, "hello"))
        cmd_hello();
    else if (streq(buf, "info"))
        cmd_info();
    else if (streq(buf, "ls"))
        cmd_ls();
    else if (streq(buf, "mtest"))
        cmd_mtest();
    else if (starts_with(buf, "mlog")) {
        char *arg = buf + 4;
        while (*arg == ' ')
            arg++;
        cmd_mlog(*arg ? arg : 0);
    }
    else if (starts_with(buf, "cat")) {
        char *arg = buf + 3;
        while (*arg == ' ')
            arg++;
        cmd_cat(*arg ? arg : 0);
    }
    else if (starts_with(buf, "heartbeat")) {
        char *arg = buf + 9;
        while (*arg == ' ')
            arg++;
        cmd_heartbeat(*arg ? arg : 0);
    }
    else if (starts_with(buf, "exec")) {
        char *arg = buf + 4;
        while (*arg == ' ')
            arg++;
        cmd_exec(*arg ? arg : 0);
    }
    else if (starts_with(buf, "setTimeout")) {
        char *arg = buf + 10;
        while (*arg == ' ')
            arg++;
        cmd_set_timeout(arg);
    }
    else if (streq(buf, "taskbatch")) {
        cmd_taskbatch();
    }
    else if (buf[0] != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(buf);
        uart_putc('\n');
    }
}

static void shell_poll_once(char *line_buf, int *line_len, int max_len, int *need_prompt) {
    char c;

    if (*need_prompt) {
        uart_puts("# ");
        *need_prompt = 0;
    }

    while (uart_getc_nonblock(&c)) {
        if (c == '\n') {
            uart_putc('\n');
            line_buf[*line_len] = '\0';
            shell_execute_command(line_buf);
            *line_len = 0;
            *need_prompt = 1;
            return;
        }

        if (c == '\b' || c == 127) {
            if (*line_len > 0) {
                (*line_len)--;
                uart_puts("\b \b");
            }
            continue;
        }

        if (*line_len < max_len - 1) {
            line_buf[(*line_len)++] = c;
            uart_putc(c);
        }
    }
}

void start_kernel(unsigned long hartid, const void *fdt) {
    (void)hartid;
    uart_init_from_dtb(fdt);

    cpio_init_from_dtb(fdt);
    mm_init(fdt);
    trap_init();
    timer_init_from_dtb(fdt);
    plic_init_from_dtb(fdt);

    plic_init();
    plic_set_priority(uart_irq_number(), 1);
    plic_enable_irq(uart_irq_number());
    task_init();
    uart_irq_init();
    enable_supervisor_external_interrupts();
    timer_init();

    uart_puts("\nNYCU OSC2026 RISC-V Kernel\n");
    uart_puts("Type 'help' for available commands.\n\n");

    char buf[128];
    int line_len = 0;
    int need_prompt = 1;

    while (1) {
        task_run_ready();
        shell_poll_once(buf, &line_len, (int)sizeof(buf), &need_prompt);
    }
}

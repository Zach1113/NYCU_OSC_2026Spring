extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);

#define SBI_EXT_BASE  0x10

#define SBI_EXT_BASE_GET_SPEC_VERSION   0
#define SBI_EXT_BASE_GET_IMP_ID         1
#define SBI_EXT_BASE_GET_IMP_VERSION    2

struct sbiret {
    long error;
    long value;
};

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

static void readline(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = uart_getc();
        if (c == '\n') {           /* Enter pressed */
            uart_putc('\n');
            break;
        }
        if (c == '\b' || c == 127) {  /* Backspace / DEL */
            if (i > 0) {
                i--;
                uart_puts("\b \b");
            }
            continue;
        }
        buf[i++] = c;
        uart_putc(c);              /* echo */
    }
    buf[i] = '\0';
}

/* strcmp without libc */
static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* Shell commands */
static void cmd_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("  help   - Show this help message\n");
    uart_puts("  hello  - Print Hello World!\n");
    uart_puts("  info   - Show OpenSBI info\n");
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

void start_kernel(void) {
    uart_puts("\nNYCU OSC2026 RISC-V Kernel\n");
    uart_puts("Type 'help' for available commands.\n\n");

    char buf[128];
    while (1) {
        uart_puts("# ");
        readline(buf, sizeof(buf));

        if (streq(buf, "help"))
            cmd_help();
        else if (streq(buf, "hello"))
            cmd_hello();
        else if (streq(buf, "info"))
            cmd_info();
        else if (buf[0] != '\0') {
            uart_puts("Unknown command: ");
            uart_puts(buf);
            uart_putc('\n');
        }
    }
}

#include "fdt.h"
#include "cpio.h"
#include "uart.h"

/* ---- Platform selection for load address ---- */
// #define PLATFORM_QEMU
#define PLATFORM_BOARD

#ifdef PLATFORM_QEMU
    #define KERNEL_LOAD_ADDR 0x80200000UL
    #define RELOC_ADDR       0x80300000UL
#else
    #define KERNEL_LOAD_ADDR 0x00200000UL
    #define RELOC_ADDR       0x20000000UL
#endif

#define LOAD_MAGIC         0x544f4f42UL /* "BOOT" in little-endian stream */
#define LOAD_MAX_SIZE      (16UL * 1024UL * 1024UL)

#define SBI_EXT_BASE  0x10

#define SBI_EXT_BASE_GET_SPEC_VERSION   0
#define SBI_EXT_BASE_GET_IMP_ID         1
#define SBI_EXT_BASE_GET_IMP_VERSION    2

struct sbiret {
    long error;
    long value;
};

struct load_header {
    unsigned long magic;
    unsigned long size;
    unsigned long checksum;
};

static const void *g_initrd_start;
static const void *g_initrd_end;
static unsigned long g_boot_hartid;
static const void *g_boot_fdt;

void start_kernel(unsigned long hartid, const void *fdt);

extern char _start[];
extern char _end[];

static unsigned long current_pc(void) {
    unsigned long pc;
    asm volatile("auipc %0, 0" : "=r"(pc));
    return pc;
}

static int range_overlap(unsigned long s1, unsigned long e1,
                         unsigned long s2, unsigned long e2) {
    return (s1 < e2) && (s2 < e1);
}

static void mem_copy(unsigned char *dst, const unsigned char *src, unsigned long n) {
    while (n--) {
        *dst++ = *src++;
    }
}

static int relocate_conflicts_with_boot_data(unsigned long dst, unsigned long size,
                                             const void *fdt) {
    unsigned long rs = dst;
    unsigned long re = dst + size;
    int off;
    int len_start = 0;
    int len_end = 0;

    if (fdt) {
        const struct fdt_header *h = (const struct fdt_header *)fdt;
        unsigned long fs = (unsigned long)fdt;
        unsigned long fe = fs + fdt_be32(&h->totalsize);
        if (range_overlap(rs, re, fs, fe))
            return 1;

        off = fdt_path_offset(fdt, "/chosen");
        if (off >= 0) {
            const void *pstart = fdt_getprop(fdt, off, "linux,initrd-start", &len_start);
            const void *pend = fdt_getprop(fdt, off, "linux,initrd-end", &len_end);
            if (pstart && pend) {
                unsigned long is = (len_start >= 8) ? fdt_be64(pstart) : fdt_be32(pstart);
                unsigned long ie = (len_end >= 8) ? fdt_be64(pend) : fdt_be32(pend);
                if (ie > is && range_overlap(rs, re, is, ie))
                    return 1;
            }
        }
    }

    return 0;
}

static int ensure_self_relocated_for_load(unsigned long resume_addr,
                                          unsigned long old_sp,
                                          const void *fdt) {
    unsigned long image_start = (unsigned long)_start;
    unsigned long image_end = (unsigned long)_end;
    unsigned long image_size = image_end - image_start;
    unsigned long pc = current_pc();
    unsigned long resume_reloc;
    unsigned long sp_reloc;

    /* Already running outside the original linked image. */
    if (pc < image_start || pc >= image_end)
        return 1;

    if (resume_addr < image_start || resume_addr >= image_end)
        return -3;

    /* Never relocate onto the currently running image. */
    if (range_overlap(RELOC_ADDR, RELOC_ADDR + image_size, image_start, image_end))
        return -2;

    if (relocate_conflicts_with_boot_data(RELOC_ADDR, image_size, fdt))
        return -1;

    mem_copy((unsigned char *)RELOC_ADDR, (const unsigned char *)image_start, image_size);

    resume_reloc = RELOC_ADDR + (resume_addr - image_start);
    if (old_sp >= image_start && old_sp < image_end)
        sp_reloc = RELOC_ADDR + (old_sp - image_start);
    else
        sp_reloc = RELOC_ADDR + image_size;

    asm volatile(
        "mv sp, %0\n"
        "fence.i\n"
        "jr %1\n"
        :
        : "r"(sp_reloc), "r"(resume_reloc)
            : "memory");

    while (1)
        ;
}

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

static unsigned long uart_get_u32_le(void) {
    unsigned long b0 = (unsigned char)uart_getc_raw();
    unsigned long b1 = (unsigned char)uart_getc_raw();
    unsigned long b2 = (unsigned char)uart_getc_raw();
    unsigned long b3 = (unsigned char)uart_getc_raw();
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void uart_read_exact(void *dst, unsigned long n, unsigned long *sum) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) {
        unsigned char v = (unsigned char)uart_getc_raw();
        *p++ = v;
        if (sum)
            *sum += v;
    }
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

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static void initrd_from_dtb(const void *fdt) {
    int off;
    int len_start = 0;
    int len_end = 0;
    const void *pstart;
    const void *pend;
    unsigned long start;
    unsigned long end;

    g_initrd_start = 0;
    g_initrd_end = 0;

    if (!fdt)
        return;

    off = fdt_path_offset(fdt, "/chosen");
    if (off < 0)
        return;

    pstart = fdt_getprop(fdt, off, "linux,initrd-start", &len_start);
    pend = fdt_getprop(fdt, off, "linux,initrd-end", &len_end);
    if (!pstart || !pend)
        return;

    start = (len_start >= 8) ? fdt_be64(pstart) : fdt_be32(pstart);
    end = (len_end >= 8) ? fdt_be64(pend) : fdt_be32(pend);
    if (end <= start)
        return;

    g_initrd_start = (const void *)start;
    g_initrd_end = (const void *)end;
    cpio_set_archive(g_initrd_start, g_initrd_end);
}

/* Shell commands */
static void cmd_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("  help   - Show this help message\n");
    uart_puts("  hello  - Print Hello World!\n");
    uart_puts("  info   - Show OpenSBI info\n");
    uart_puts("  ls     - List files in initramfs cpio\n");
    uart_puts("  cat    - Print file content from initramfs (usage: cat <name>)\n");
    uart_puts("  load   - Load a kernel image over UART and jump to it\n");
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

static void cmd_load(void) {
    int reloc_status;
    unsigned long sp_now;
    unsigned long pc;

    unsigned long image_start = (unsigned long)_start;
    unsigned long image_end = (unsigned long)_end;

    asm volatile("mv %0, sp" : "=r"(sp_now));
    reloc_status = ensure_self_relocated_for_load((unsigned long)&&after_reloc,
                                                  sp_now,
                                                  g_boot_fdt);

after_reloc:
    pc = current_pc();
    /* If we resumed in relocated image, ignore pre-jump stack locals. */
    if (pc < image_start || pc >= image_end)
        reloc_status = 1;

    if (reloc_status < 0) {
        if (reloc_status == -2)
            uart_puts("load: relocation skipped (destination overlaps source image)\n");
        else
            uart_puts("load: relocation skipped due to overlap risk\n");
        return;
    }

    {
    struct load_header hdr;
    unsigned long sum = 0;

    uart_puts("Send header: <magic:u32><size:u32><checksum:u32> then payload\n");
    uart_puts("Waiting for stream...\n");

    hdr.magic = uart_get_u32_le();
    hdr.size = uart_get_u32_le();
    hdr.checksum = uart_get_u32_le();

    if (hdr.magic != LOAD_MAGIC) {
        uart_puts("load: bad magic, expected ");
        uart_hex(LOAD_MAGIC);
        uart_puts(", got ");
        uart_hex(hdr.magic);
        uart_putc('\n');
        return;
    }

    if (hdr.size == 0 || hdr.size > LOAD_MAX_SIZE) {
        uart_puts("load: invalid size ");
        uart_hex(hdr.size);
        uart_putc('\n');
        return;
    }

    uart_puts("Receiving ");
    uart_hex(hdr.size);
    uart_puts(" bytes to ");
    uart_hex(KERNEL_LOAD_ADDR);
    uart_putc('\n');

    uart_read_exact((void *)KERNEL_LOAD_ADDR, hdr.size, &sum);

    if (sum != hdr.checksum) {
        uart_puts("load: checksum mismatch expected=");
        uart_hex(hdr.checksum);
        uart_puts(" got=");
        uart_hex(sum);
        uart_putc('\n');
        return;
    }

    uart_puts("load: transfer complete, jumping to ");
    uart_hex(KERNEL_LOAD_ADDR);
    uart_putc('\n');

    asm volatile(
        "mv a0, %0\n"
        "mv a1, %1\n"
        "fence.i\n"
        "jr %2\n"
        :
        : "r"(g_boot_hartid), "r"(g_boot_fdt), "r"(KERNEL_LOAD_ADDR)
        : "a0", "a1", "memory");

    while (1)
        ;
    }
}

static void cmd_ls(void) {
    cpio_ls();
}

static void cmd_cat(const char *name) {
    cpio_cat(name);
}

void start_kernel(unsigned long hartid, const void *fdt) {
    g_boot_hartid = hartid;
    g_boot_fdt = fdt;
    uart_init_from_dtb(fdt);

    initrd_from_dtb(fdt);

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
        else if (streq(buf, "ls"))
            cmd_ls();
        else if (starts_with(buf, "cat")) {
            char *arg = buf + 3;
            while (*arg == ' ')
                arg++;
            cmd_cat(*arg ? arg : 0);
        }
        else if (streq(buf, "load"))
            cmd_load();
        else if (buf[0] != '\0') {
            uart_puts("Unknown command: ");
            uart_puts(buf);
            uart_putc('\n');
        }
    }
}

#include "fdt.h"
#include "uart.h"

#define KERNEL_LOAD_ADDR 0x00200000UL
#define RELOC_ADDR       0x20000000UL
#define LOAD_MAGIC       0x544F4F42UL /* "BOOT" */
#define LOAD_MAX_SIZE    (16UL * 1024UL * 1024UL)

struct load_header {
    unsigned long magic;
    unsigned long size;
    unsigned long checksum;
};

static unsigned long g_boot_hartid;
static const void *g_boot_fdt;

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

    if (pc < image_start || pc >= image_end)
        return 1;

    if (resume_addr < image_start || resume_addr >= image_end)
        return -3;

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
        if (c == '\n') {
            uart_putc('\n');
            break;
        }
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                uart_puts("\b \b");
            }
            continue;
        }
        buf[i++] = c;
        uart_putc(c);
    }
    buf[i] = '\0';
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void cmd_help(void) {
    uart_puts("Available commands:\n");
    uart_puts("  load - Receive kernel stream and jump to it\n");
    uart_puts("  help - Show this help message\n");
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
    unsigned long sum32;

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
    sum32 = sum & 0xFFFFFFFFUL;

    if (sum32 != hdr.checksum) {
        uart_puts("load: checksum mismatch expected=");
        uart_hex(hdr.checksum);
        uart_puts(" got=");
        uart_hex(sum32);
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

void start_kernel(unsigned long hartid, const void *fdt) {
    char buf[64];

    g_boot_hartid = hartid;
    g_boot_fdt = fdt;

    uart_init_from_dtb(fdt);
    uart_puts("\nSimple Bootloader\n");
    uart_puts("Type 'load' to receive a kernel over UART.\n");
    uart_puts("Type 'help' for commands.\n\n");

    while (1) {
        uart_puts("# ");
        readline(buf, sizeof(buf));

        if (streq(buf, "load"))
            cmd_load();
        else if (streq(buf, "help"))
            cmd_help();
        else if (buf[0] != '\0') {
            uart_puts("Unknown command: ");
            uart_puts(buf);
            uart_putc('\n');
        }
    }
}

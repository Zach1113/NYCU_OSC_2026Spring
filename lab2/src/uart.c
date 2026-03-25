/*
 * QEMU virt:     base = 0x10000000, byte-width registers, LSR at offset 0x5
 * OrangePi RV2:  base = 0xD4017000, 32-bit registers,     LSR at offset 0x14
 */

#include "fdt.h"
#include "uart.h"

/* ---- Platform selection ---- */
// #define PLATFORM_QEMU
#define PLATFORM_BOARD  /* OrangePi RV2 */

#ifdef PLATFORM_QEMU
    #define UART_BASE_DEFAULT  0x10000000UL
    static int uart_reg32 = 0;
#else /* PLATFORM_BOARD */
    #define UART_BASE_DEFAULT  0xD4017000UL
    static int uart_reg32 = 1;
#endif

static unsigned long uart_base = UART_BASE_DEFAULT;

static unsigned int uart_read_reg(unsigned long off) {
        if (uart_reg32)
                return *(volatile unsigned int *)(uart_base + off);
        return *(volatile unsigned char *)(uart_base + off);
}

static void uart_write_reg(unsigned long off, unsigned int v) {
        if (uart_reg32)
                *(volatile unsigned int *)(uart_base + off) = v;
        else
                *(volatile unsigned char *)(uart_base + off) = (unsigned char)v;
}

static unsigned long uart_lsr_off(void) {
        return uart_reg32 ? 0x14UL : 0x05UL;
}

static unsigned long uart_data_off(void) {
        return 0x00UL;
}

#define LSR_DR     (1 << 0)   /* bit 0: Data Ready (RX available) */
#define LSR_TDRQ   (1 << 5)   /* bit 5: TX Data Request (TX empty) */

void uart_init_from_dtb(const void *fdt) {
    int off;
    int len = 0;
    const void *prop;

    if (!fdt)
        return;

    off = fdt_path_offset(fdt, "/soc/serial");
    if (off < 0)
        off = fdt_path_offset(fdt, "/soc/serial@10000000");
    if (off < 0)
        off = fdt_path_offset(fdt, "/soc/serial@d4017000");
    if (off < 0)
        return;

    prop = fdt_getprop(fdt, off, "reg", &len);
    if (!prop)
        return;

    if (len >= 8)
        uart_base = fdt_be64(prop);
    else if (len >= 4)
        uart_base = fdt_be32(prop);
    else
        return;

    /* Switch register layout by common platform UART base. */
    if (uart_base == 0x10000000UL)
        uart_reg32 = 0;
    else
        uart_reg32 = 1;
}

unsigned long uart_base_addr(void) {
    return uart_base;
}

/* Read one raw byte (blocking) */
char uart_getc_raw(void) {
    while ((uart_read_reg(uart_lsr_off()) & LSR_DR) == 0)
        ;
    return (char)(uart_read_reg(uart_data_off()) & 0xFF);
}

unsigned long uart_get_u32_le(void) {
    unsigned long b0 = (unsigned char)uart_getc_raw();
    unsigned long b1 = (unsigned char)uart_getc_raw();
    unsigned long b2 = (unsigned char)uart_getc_raw();
    unsigned long b3 = (unsigned char)uart_getc_raw();
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void uart_read_exact(void *dst, unsigned long n, unsigned long *sum) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) {
        unsigned char v = (unsigned char)uart_getc_raw();
        *p++ = v;
        if (sum)
            *sum += v;
    }
}

/* Read one character (blocking) */
char uart_getc(void) {
    char c = uart_getc_raw();
    return c == '\r' ? '\n' : c;   /* normalize CR -> LF */
}

/* Write one character (blocking) */
void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');           /* expand LF -> CR+LF for terminals */
    while ((uart_read_reg(uart_lsr_off()) & LSR_TDRQ) == 0)
        ;
    uart_write_reg(uart_data_off(), (unsigned int)c);
}

/* Write a null-terminated string */
void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

/* Print unsigned long as decimal */
void uart_dec(unsigned long v) {
    char buf[21];
    int i = 0;

    if (v == 0) {
        uart_putc('0');
        return;
    }

    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }

    while (i-- > 0)
        uart_putc(buf[i]);
}

/* Print unsigned long as 16-digit hex */
void uart_hex(unsigned long h) {
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned long n = (h >> shift) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc((char)n);
    }
}

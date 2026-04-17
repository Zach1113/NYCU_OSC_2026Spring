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
static int uart_irq = 42;
static int uart_irq_mode;
static int uart_rx_irq_seen;
static int uart_tx_irq_seen;

struct ring_buffer {
    char buf[256];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
};

static struct ring_buffer g_rx_rb;
static struct ring_buffer g_tx_rb;

static void rb_init(struct ring_buffer *rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

static int rb_empty(const struct ring_buffer *rb) {
    return rb->count == 0;
}

static int rb_full(const struct ring_buffer *rb) {
    return rb->count == sizeof(rb->buf);
}

static int rb_push(struct ring_buffer *rb, char c) {
    if (rb_full(rb))
        return 0;
    rb->buf[rb->tail] = c;
    rb->tail = (rb->tail + 1U) % (unsigned int)sizeof(rb->buf);
    rb->count++;
    return 1;
}

static int rb_pop(struct ring_buffer *rb, char *out) {
    if (rb_empty(rb))
        return 0;
    *out = rb->buf[rb->head];
    rb->head = (rb->head + 1U) % (unsigned int)sizeof(rb->buf);
    rb->count--;
    return 1;
}

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

/* Line Status Register:
 * - bit0: RX data ready
 * - bit5: TX holding register empty / ready for next byte
 */
static unsigned long uart_lsr_off(void) {
        return uart_reg32 ? 0x14UL : 0x05UL;
}

/* Interrupt Enable Register:
 * controls whether RX/TX related UART events can raise interrupts.
 */
static unsigned long uart_ier_off(void) {
        return uart_reg32 ? 0x04UL : 0x01UL;
}

/* Interrupt Identification Register:
 * tells the handler why UART interrupted us.
 */
static unsigned long uart_iir_off(void) {
        return uart_reg32 ? 0x08UL : 0x02UL;
}

/* Transmit/Receive data register:
 * reading gets one received byte, writing sends one byte.
 */
static unsigned long uart_data_off(void) {
        return 0x00UL;
}

#define LSR_DR     (1 << 0)   /* bit 0: Data Ready (RX available) */
#define LSR_TDRQ   (1 << 5)   /* bit 5: TX Data Request (TX empty) */

#define IER_RX_INT (1U << 0)
#define IER_TX_INT (1U << 1)

#define IIR_NO_INT (1U << 0)

#define SSTATUS_SIE (1UL << 1)

static unsigned long irq_save(void) {
    unsigned long sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrci sstatus, 2");
    return sstatus;
}

static void irq_restore(unsigned long sstatus) {
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

static unsigned int uart_read_ier(void) {
    return uart_read_reg(uart_ier_off());
}

static void uart_write_ier(unsigned int v) {
    uart_write_reg(uart_ier_off(), v);
}

static void uart_enable_rx_irq(void) {
    uart_write_ier(uart_read_ier() | IER_RX_INT);
}

static void uart_enable_tx_irq(void) {
    uart_write_ier(uart_read_ier() | IER_TX_INT);
}

static void uart_disable_tx_irq(void) {
    uart_write_ier(uart_read_ier() & ~IER_TX_INT);
}

static int uart_try_hw_getc(char *out) {
    if ((uart_read_reg(uart_lsr_off()) & LSR_DR) == 0)
        return 0;
    *out = (char)(uart_read_reg(uart_data_off()) & 0xFF);
    if (*out == '\r')
        *out = '\n';
    return 1;
}

static int uart_try_hw_putc_raw(char c) {
    if ((uart_read_reg(uart_lsr_off()) & LSR_TDRQ) == 0)
        return 0;
    uart_write_reg(uart_data_off(), (unsigned int)c);
    return 1;
}

static void uart_tx_kick_locked(void) {
    char c;

    while (!rb_empty(&g_tx_rb)) {
        if (!uart_try_hw_putc_raw(g_tx_rb.buf[g_tx_rb.head]))
            break;
        rb_pop(&g_tx_rb, &c);
    }

    if (rb_empty(&g_tx_rb))
        uart_disable_tx_irq();
    else
        uart_enable_tx_irq();
}

static void uart_tx_flush_poll(void) {
    char c;

    while (rb_pop(&g_tx_rb, &c))
        uart_putc_poll(c);
}

static void uart_tx_enqueue_blocking(char c) {
    unsigned long flags;

    while (1) {
        flags = irq_save();
        uart_tx_kick_locked();
        if (rb_push(&g_tx_rb, c)) {
            uart_enable_tx_irq();
            uart_tx_kick_locked();
            irq_restore(flags);

            /* Some platforms do not deliver TX-empty interrupts reliably during
             * bring-up. Until we observe one, synchronously flush the queue so
             * early boot messages do not get stuck in chunks.
             */
            if (!uart_tx_irq_seen)
                uart_tx_flush_poll();
            return;
        }
        irq_restore(flags);

        /* Buffer is full. Keep helping TX progress even if TX IRQ delivery is
         * delayed or unavailable on the current platform configuration.
         */
        while (rb_full(&g_tx_rb)) {
            flags = irq_save();
            uart_tx_kick_locked();
            irq_restore(flags);
        }
    }
}

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

    prop = fdt_getprop(fdt, off, "interrupts", &len);
    if (prop && len >= 4)
        uart_irq = (int)fdt_be32(prop);
}

unsigned long uart_base_addr(void) {
    return uart_base;
}

int uart_irq_number(void) {
    return uart_irq;
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

char uart_getc_poll(void) {
    char c = uart_getc_raw();
    return c == '\r' ? '\n' : c;
}

int uart_getc_nonblock(char *out) {
    unsigned long flags;

    if (!out)
        return 0;

    if (!uart_irq_mode)
        return uart_try_hw_getc(out);

    flags = irq_save();
    if (rb_pop(&g_rx_rb, out)) {
        irq_restore(flags);
        return 1;
    }
    irq_restore(flags);

    /*
     * Keep the shell usable during bring-up if RX IRQ delivery is not active
     * yet on the current platform configuration.
     */
    if (!uart_rx_irq_seen)
        return uart_try_hw_getc(out);

    return 0;
}

/* Read one character (blocking) */
char uart_getc(void) {
    char c;

    if (!uart_irq_mode)
        return uart_getc_poll();

    while (!uart_getc_nonblock(&c))
        ;
    return c;
}

/* Write one character (blocking) */
void uart_putc_poll(char c) {
    if (c == '\n')
        uart_putc_poll('\r');
    while ((uart_read_reg(uart_lsr_off()) & LSR_TDRQ) == 0)
        ;
    uart_write_reg(uart_data_off(), (unsigned int)c);
}

void uart_putc(char c) {
    if (!uart_irq_mode) {
        uart_putc_poll(c);
        return;
    }

    if (c == '\n')
        uart_tx_enqueue_blocking('\r');
    uart_tx_enqueue_blocking(c);
}

/* Write a null-terminated string */
void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void uart_irq_init(void) {
    rb_init(&g_rx_rb);
    rb_init(&g_tx_rb);
    uart_irq_mode = 1;
    uart_rx_irq_seen = 0;
    uart_tx_irq_seen = 0;

    /* Common 16550-style FIFO init value:
     * bit0 = enable FIFO
     * bit1 = clear RX FIFO
     * bit2 = clear TX FIFO
     */
    uart_write_reg(uart_iir_off(), 0x07U);
    uart_enable_rx_irq();
    uart_disable_tx_irq();
}

void uart_irq_handler(void) {
    unsigned int iir;
    int budget = 64;

    while (budget-- > 0) {
        iir = uart_read_reg(uart_iir_off()) & 0x0FU;
        if (iir & IIR_NO_INT)
            break;

        /* 0x04: RX data available
         * 0x0c: character timeout while data remains in RX FIFO
         * 0x06: line status change (this code also drains RX data just in case)
         */
        if (iir == 0x04U || iir == 0x0CU || iir == 0x06U) {
            char c;
            uart_rx_irq_seen = 1;
            while (uart_try_hw_getc(&c)) {
                if (!rb_push(&g_rx_rb, c))
                    break;
            }
            continue;
        }

        /* 0x02: TX holding register empty, so feed the next queued bytes. */
        if (iir == 0x02U) {
            unsigned long flags = irq_save();

            uart_tx_irq_seen = 1;
            uart_tx_kick_locked();
            irq_restore(flags);
            continue;
        }

        break;
    }
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

#ifndef UART_H
#define UART_H

void uart_init_from_dtb(const void *fdt);
unsigned long uart_base_addr(void);
int uart_irq_number(void);
char uart_getc_raw(void);
int uart_getc_nonblock(char *out);
unsigned long uart_get_u32_le(void);
void uart_read_exact(void *dst, unsigned long n, unsigned long *sum);
char uart_getc(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_irq_init(void);
void uart_irq_handler(void);
void uart_dec(unsigned long v);
void uart_hex(unsigned long h);

#endif

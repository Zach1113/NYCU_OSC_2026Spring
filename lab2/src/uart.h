#ifndef UART_H
#define UART_H

void uart_init_from_dtb(const void *fdt);
unsigned long uart_base_addr(void);
char uart_getc_raw(void);
char uart_getc(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_dec(unsigned long v);
void uart_hex(unsigned long h);

#endif

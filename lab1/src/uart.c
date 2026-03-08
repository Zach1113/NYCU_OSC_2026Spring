/*
 * uart.c - Exercise 2: UART driver
 *
 * QEMU virt:     base = 0x10000000, byte-width registers, LSR at offset 0x5
 * OrangePi RV2:  base = 0xD4017000, 32-bit registers,     LSR at offset 0x14
 *
 * Toggle the #defines below when switching platforms.
 */

/* ---- Platform selection ---- */
/* #define PLATFORM_QEMU */
#define PLATFORM_BOARD  /* OrangePi RV2 */

#ifdef PLATFORM_QEMU
  #define UART_BASE  0x10000000UL
  #define UART_RBR   ((volatile unsigned char *)(UART_BASE + 0x00))
  #define UART_THR   ((volatile unsigned char *)(UART_BASE + 0x00))
  #define UART_LSR   ((volatile unsigned char *)(UART_BASE + 0x05))
#else /* PLATFORM_BOARD */
  #define UART_BASE  0xD4017000UL
  #define UART_RBR   ((volatile unsigned int *)(UART_BASE + 0x00))
  #define UART_THR   ((volatile unsigned int *)(UART_BASE + 0x00))
  #define UART_LSR   ((volatile unsigned int *)(UART_BASE + 0x14))
#endif

#define LSR_DR     (1 << 0)   /* bit 0: Data Ready (RX available) */
#define LSR_TDRQ   (1 << 5)   /* bit 5: TX Data Request (TX empty) */

/* Read one character (blocking) */
char uart_getc(void) {
    while ((*UART_LSR & LSR_DR) == 0)
        ;
    char c = (char)(*UART_RBR & 0xFF);
    return c == '\r' ? '\n' : c;   /* normalize CR -> LF */
}

/* Write one character (blocking) */
void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');           /* expand LF -> CR+LF for terminals */
    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
    *UART_THR = c;
}

/* Write a null-terminated string */
void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

/* Print unsigned long as 16-digit hex (e.g. 0x0000000000000002) */
void uart_hex(unsigned long h) {
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned long n = (h >> shift) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc((char)n);
    }
}

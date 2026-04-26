#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

typedef void (*timer_callback_t)(void *arg);

unsigned long long timer_now(void);
unsigned long timer_seconds_since_boot(void);
void timer_init_from_dtb(const void *fdt);
void timer_init(void);
int add_timer(timer_callback_t callback, void *arg, int sec);
void timer_handle_interrupt(void);
void timer_set_periodic_enabled(int enabled);
int timer_periodic_enabled(void);

#endif
